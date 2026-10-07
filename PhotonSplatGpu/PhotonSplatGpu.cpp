#include "PhotonSplatGpu.h"
#include "../../CGLib/Math/Vector4d.h"
#include "../../CGLib/VulkanGraphics/VulkanContext.h"
#include "../../CGLib/VulkanGraphics/VulkanCommandPool.h"
#include "../../CGLib/VulkanGraphics/VulkanPipeline.h"
#include "../../CGLib/VulkanGraphics/VulkanSPVLoader.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>

namespace Phantom::RayTracer {
namespace {
using V= Math::Vector3dd;
using F= Math::Vector3df;
using F4= Math::Vector4df;
constexpr VkFormat colorFormat=VK_FORMAT_R32G32B32A32_SFLOAT;
constexpr VkFormat depthFormat=VK_FORMAT_D32_SFLOAT;
bool finite(V p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
V vector(const double* p) { return V(p[0],p[1],p[2]); }
double elapsed(std::chrono::steady_clock::time_point start)
{ return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(); }
struct Camera { F4 eye,right,up,forward,projection,dimensions; };
struct Triangle { F4 a,b,c,normal,reflectance; };
struct GpuPhoton { F4 position,direction,normal,power; };
static_assert(sizeof(Camera)==96 && sizeof(Triangle)==80 && sizeof(GpuPhoton)==64);

// Explicit coherent allocations avoid relying on mapped VMA memory being coherent.
struct Buffer {
    VkDevice device=VK_NULL_HANDLE;
    VkBuffer buffer=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    void* mapped=nullptr;
    VkDeviceSize capacity=0;
    VkBufferUsageFlags bufferUsage=0;
    bool cachedReadback=false;
    ~Buffer() { destroy(); }
    void destroy() {
        if(mapped) vkUnmapMemory(device,memory);
        if(buffer) vkDestroyBuffer(device,buffer,nullptr);
        if(memory) vkFreeMemory(device,memory,nullptr);
        mapped=nullptr; buffer=VK_NULL_HANDLE; memory=VK_NULL_HANDLE;
        capacity=0; bufferUsage=0;
    }
    bool create(const VKG::VulkanContext& ctx,VkDeviceSize size,VkBufferUsageFlags usage,bool readback=false) {
        if(buffer && mapped && device==ctx.getDevice() && capacity>=size && bufferUsage==usage && cachedReadback==readback) return true;
        destroy(); device=ctx.getDevice();
        VkBufferCreateInfo ci{}; ci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO; ci.size=size;
        ci.usage=usage; ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if(vkCreateBuffer(device,&ci,nullptr,&buffer)!=VK_SUCCESS) return false;
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,buffer,&req);
        const VkMemoryPropertyFlags flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        auto type=ctx.findMemoryType(req.memoryTypeBits,flags|(readback?VK_MEMORY_PROPERTY_HOST_CACHED_BIT:VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
        if(!type) type=ctx.findMemoryType(req.memoryTypeBits,flags);
        if(!type) return false;
        VkMemoryAllocateInfo ai{}; ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize=req.size; ai.memoryTypeIndex=*type;
        if(vkAllocateMemory(device,&ai,nullptr,&memory)!=VK_SUCCESS
            || vkBindBufferMemory(device,buffer,memory,0)!=VK_SUCCESS
            || vkMapMemory(device,memory,0,size,0,&mapped)!=VK_SUCCESS) return false;
        capacity=size; bufferUsage=usage; cachedReadback=readback; return true;
    }
};
struct Target {
    VkDevice device=VK_NULL_HANDLE;
    VkImage color=VK_NULL_HANDLE,depth=VK_NULL_HANDLE;
    VkDeviceMemory colorMemory=VK_NULL_HANDLE,depthMemory=VK_NULL_HANDLE;
    VkImageView colorView=VK_NULL_HANDLE,depthView=VK_NULL_HANDLE;
    VkRenderPass pass=VK_NULL_HANDLE;
    VkFramebuffer framebuffer=VK_NULL_HANDLE;
    ~Target() { destroy(); }
    void destroy() {
        if(framebuffer) vkDestroyFramebuffer(device,framebuffer,nullptr);
        if(pass) vkDestroyRenderPass(device,pass,nullptr);
        if(colorView) vkDestroyImageView(device,colorView,nullptr);
        if(depthView) vkDestroyImageView(device,depthView,nullptr);
        if(color) vkDestroyImage(device,color,nullptr);
        if(depth) vkDestroyImage(device,depth,nullptr);
        if(colorMemory) vkFreeMemory(device,colorMemory,nullptr);
        if(depthMemory) vkFreeMemory(device,depthMemory,nullptr);
        framebuffer=VK_NULL_HANDLE; pass=VK_NULL_HANDLE; colorView=depthView=VK_NULL_HANDLE;
        color=depth=VK_NULL_HANDLE; colorMemory=depthMemory=VK_NULL_HANDLE;
    }
    bool image(const VKG::VulkanContext& ctx,int w,int h,VkFormat format,VkImageUsageFlags usage,
               VkImageAspectFlags aspect,VkImage& image,VkDeviceMemory& memory,VkImageView& view) {
        VkImageCreateInfo ci{}; ci.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO; ci.imageType=VK_IMAGE_TYPE_2D;
        ci.format=format; ci.extent={static_cast<uint32_t>(w),static_cast<uint32_t>(h),1};
        ci.mipLevels=ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT;
        ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.usage=usage; ci.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(vkCreateImage(device,&ci,nullptr,&image)!=VK_SUCCESS) return false;
        VkMemoryRequirements req; vkGetImageMemoryRequirements(device,image,&req);
        const auto type=ctx.findMemoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if(!type) return false;
        VkMemoryAllocateInfo ai{}; ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize=req.size; ai.memoryTypeIndex=*type;
        if(vkAllocateMemory(device,&ai,nullptr,&memory)!=VK_SUCCESS || vkBindImageMemory(device,image,memory,0)!=VK_SUCCESS) return false;
        VkImageViewCreateInfo vi{}; vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO; vi.image=image;
        vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=format; vi.subresourceRange={aspect,0,1,0,1};
        return vkCreateImageView(device,&vi,nullptr,&view)==VK_SUCCESS;
    }
    bool create(const VKG::VulkanContext& ctx,int w,int h,bool geometry) {
        destroy(); device=ctx.getDevice();
        if(!image(ctx,w,h,colorFormat,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,color,colorMemory,colorView)) return false;
        if(geometry && !image(ctx,w,h,depthFormat,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT,depth,depthMemory,depthView)) return false;
        VkAttachmentDescription attachments[2]{};
        attachments[0].format=colorFormat; attachments[0].samples=VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; attachments[0].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachments[0].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout=geometry?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        attachments[1]=attachments[0]; attachments[1].format=depthFormat;
        attachments[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        VkAttachmentReference colorRef{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},depthRef{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount=1; sub.pColorAttachments=&colorRef; if(geometry) sub.pDepthStencilAttachment=&depthRef;
        VkSubpassDependency dependencies[2]{};
        dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL; dependencies[0].dstSubpass=0;
        dependencies[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dependencies[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        dependencies[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass=0; dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask=dependencies[0].dstStageMask;
        dependencies[1].dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[1].srcAccessMask=dependencies[0].dstAccessMask;
        dependencies[1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rp{}; rp.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rp.attachmentCount=geometry?2:1; rp.pAttachments=attachments; rp.subpassCount=1; rp.pSubpasses=&sub;
        rp.dependencyCount=2; rp.pDependencies=dependencies;
        if(vkCreateRenderPass(device,&rp,nullptr,&pass)!=VK_SUCCESS) return false;
        const VkImageView views[]={colorView,depthView};
        VkFramebufferCreateInfo fb{}; fb.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb.renderPass=pass; fb.attachmentCount=rp.attachmentCount; fb.pAttachments=views;
        fb.width=w; fb.height=h; fb.layers=1;
        return vkCreateFramebuffer(device,&fb,nullptr,&framebuffer)==VK_SUCCESS;
    }
};
}
struct PhotonSplatGpu::Impl {
    VKG::VulkanContext* context=nullptr;
    const VKG::VulkanCommandPool* pool=nullptr;
    VkDescriptorSetLayout layout=VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool=VK_NULL_HANDLE;
    VkDescriptorSet descriptor=VK_NULL_HANDLE;
    VkSampler sampler=VK_NULL_HANDLE;
    VkQueryPool queries=VK_NULL_HANDLE;
    VKG::VulkanPipeline geometryPipeline,splatPipeline;
    VKG::PipelineConfig geometryConfig,splatConfig,paraboloidConfig;
    Target geometry,accumulation;
    Buffer cameraBuffer,trianglesBuffer,photonsBuffer,readback;
    int width=0,height=0;
    bool ready=false;
    bool paraboloidTarget=false;
    std::string error;
    PhotonSplatStats stats;
    bool fail(const char* message) { error=message; return false; }
};
PhotonSplatGpu::PhotonSplatGpu() : impl_(std::make_unique<Impl>()) {}
PhotonSplatGpu::~PhotonSplatGpu() { destroy(); }
const std::string& PhotonSplatGpu::getLastError() const { return impl_->error; }
const PhotonSplatStats& PhotonSplatGpu::getStats() const { return impl_->stats; }
void PhotonSplatGpu::destroy()
{
    auto& s=*impl_; if(!s.context) return;
    const VkDevice device=s.context->getDevice();
    s.geometryPipeline.destroy(device); s.splatPipeline.destroy(device);
    s.geometry.destroy(); s.accumulation.destroy();
    s.cameraBuffer.destroy(); s.trianglesBuffer.destroy(); s.photonsBuffer.destroy(); s.readback.destroy();
    if(s.queries) vkDestroyQueryPool(device,s.queries,nullptr);
    if(s.sampler) vkDestroySampler(device,s.sampler,nullptr);
    if(s.descriptorPool) vkDestroyDescriptorPool(device,s.descriptorPool,nullptr);
    if(s.layout) vkDestroyDescriptorSetLayout(device,s.layout,nullptr);
    s.queries=VK_NULL_HANDLE; s.sampler=VK_NULL_HANDLE; s.descriptorPool=VK_NULL_HANDLE;
    s.layout=VK_NULL_HANDLE; s.descriptor=VK_NULL_HANDLE; s.context=nullptr; s.pool=nullptr;
    s.width=s.height=0; s.ready=false;
}
bool PhotonSplatGpu::create(VKG::VulkanContext& context,const VKG::VulkanCommandPool& pool,const std::string& directory)
{
    destroy(); auto& s=*impl_; s.error.clear();
    if(!context.getDevice() || !pool.get()) return s.fail("Uninitialized Vulkan context or pool");
    s.context=&context; s.pool=&pool; const VkDevice device=context.getDevice();
    VkFormatProperties color{},depth{};
    vkGetPhysicalDeviceFormatProperties(context.getPhysicalDevice(),colorFormat,&color);
    vkGetPhysicalDeviceFormatProperties(context.getPhysicalDevice(),depthFormat,&depth);
    const VkFormatFeatureFlags colorNeed=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    const VkFormatFeatureFlags depthNeed=VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if((color.optimalTilingFeatures&colorNeed)!=colorNeed || (depth.optimalTilingFeatures&depthNeed)!=depthNeed) return s.fail("Required RGBA32F blending or D32 sampled depth unsupported");
    VkDescriptorSetLayoutBinding bindings[5]{};
    for(uint32_t i=0;i<5;++i) {
        bindings[i].binding=i; bindings[i].descriptorCount=1;
        bindings[i].descriptorType=i==0?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:(i<3?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        bindings[i].stageFlags=VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{}; li.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount=5; li.pBindings=bindings;
    if(vkCreateDescriptorSetLayout(device,&li,nullptr,&s.layout)!=VK_SUCCESS) return s.fail("Descriptor layout creation failed");
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2}};
    VkDescriptorPoolCreateInfo dp{}; dp.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets=1; dp.poolSizeCount=3; dp.pPoolSizes=sizes;
    if(vkCreateDescriptorPool(device,&dp,nullptr,&s.descriptorPool)!=VK_SUCCESS) return s.fail("Descriptor pool creation failed");
    VkDescriptorSetAllocateInfo da{}; da.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    da.descriptorPool=s.descriptorPool; da.descriptorSetCount=1; da.pSetLayouts=&s.layout;
    if(vkAllocateDescriptorSets(device,&da,&s.descriptor)!=VK_SUCCESS) return s.fail("Descriptor allocation failed");
    VkSamplerCreateInfo si{}; si.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter=si.minFilter=VK_FILTER_NEAREST; si.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU=si.addressModeV=si.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(vkCreateSampler(device,&si,nullptr,&s.sampler)!=VK_SUCCESS) return s.fail("Sampler creation failed");
    if(context.getTimestampPeriodNs()>0) {
        VkQueryPoolCreateInfo qi{}; qi.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        qi.queryType=VK_QUERY_TYPE_TIMESTAMP; qi.queryCount=4;
        if(vkCreateQueryPool(device,&qi,nullptr,&s.queries)!=VK_SUCCESS) return s.fail("Timestamp creation failed");
    }
    const auto dir=std::filesystem::path(directory);
    s.geometryConfig={}; s.geometryConfig.vertSpv=VKG::loadSPV((dir/"gbuffer.vert.spv").string());
    s.geometryConfig.fragSpv=VKG::loadSPV((dir/"gbuffer.frag.spv").string());
    s.geometryConfig.descriptorSetLayout=s.layout; s.geometryConfig.cullMode=VK_CULL_MODE_NONE;
    s.paraboloidConfig=s.geometryConfig;
    s.paraboloidConfig.vertSpv=VKG::loadSPV((dir/"paraboloid.vert.spv").string());
    s.paraboloidConfig.fragSpv=VKG::loadSPV((dir/"paraboloid.frag.spv").string());
    s.splatConfig={}; s.splatConfig.vertSpv=VKG::loadSPV((dir/"splat.vert.spv").string());
    s.splatConfig.fragSpv=VKG::loadSPV((dir/"splat.frag.spv").string());
    s.splatConfig.descriptorSetLayout=s.layout; s.splatConfig.cullMode=VK_CULL_MODE_NONE;
    s.splatConfig.depthTest=s.splatConfig.depthWrite=false;
    s.splatConfig.blendEnable=s.splatConfig.additiveBlend=true;
    if(s.geometryConfig.vertSpv.empty() || s.geometryConfig.fragSpv.empty() || s.splatConfig.vertSpv.empty() || s.splatConfig.fragSpv.empty()) return s.fail("Missing photon-splat SPIR-V shaders");
    s.ready=true; return true;
}
bool PhotonSplatGpu::render(const std::vector<RtTriangle>& input,const PhotonMap& map,
    const RtCameraSpec& camera,int width,int height,double radius,Graphics::Imagef& indirect,
    PhotonGBuffer& receivers,PhotonContribution contribution,double nearPlane,double farPlane)
{
    return renderImpl(input,map,camera,width,height,radius,indirect,receivers,contribution,nearPlane,farPlane,false);
}
bool PhotonSplatGpu::rasterize(const std::vector<RtTriangle>& triangles,const RtCameraSpec& camera,
    int width,int height,PhotonGBuffer& receivers,double nearPlane,double farPlane)
{
    PhotonMap empty; Graphics::Imagef unused;
    return renderImpl(triangles,empty,camera,width,height,1,unused,receivers,
        PhotonContribution::All,nearPlane,farPlane,true);
}
bool PhotonSplatGpu::rasterizeParaboloid(const std::vector<RtTriangle>& triangles,const RtCameraSpec& camera,
    int resolution,PhotonGBuffer& receivers,double nearPlane,double farPlane,int subdivision)
{
    PhotonMap empty; Graphics::Imagef unused;
    return renderImpl(triangles,empty,camera,resolution,resolution,1,unused,receivers,
        PhotonContribution::All,nearPlane,farPlane,true,true,subdivision);
}
bool PhotonSplatGpu::renderImpl(const std::vector<RtTriangle>& input,const PhotonMap& map,
    const RtCameraSpec& camera,int width,int height,double radius,Graphics::Imagef& indirect,
    PhotonGBuffer& receivers,PhotonContribution contribution,double nearPlane,double farPlane,bool geometryOnly,
    bool paraboloid,int subdivision)
{
    auto& s=*impl_; s.stats={}; s.error.clear(); const auto uploadStart=std::chrono::steady_clock::now();
    const V eye=vector(camera.lookFrom),target=vector(camera.lookAt),up=vector(camera.up);
    if(!s.ready || width<1 || height<1 || width>4096 || height>4096 || input.empty() || input.size()>1000000
        || !finite(eye) || !finite(target) || !finite(up) || !finite(target-eye)
        || !std::isfinite(radius) || radius<=0 || radius>1e10 || !std::isfinite(nearPlane) || !std::isfinite(farPlane)
        || nearPlane<=0 || farPlane<=nearPlane || farPlane>1e10 || farPlane/nearPlane>1e7
        || !std::isfinite(camera.fovDeg) || camera.fovDeg<=0 || camera.fovDeg>=179
        || glm::length(target-eye)<1e-10 || glm::length(glm::cross(target-eye,up))<1e-10
        || (contribution!=PhotonContribution::All && contribution!=PhotonContribution::Direct && contribution!=PhotonContribution::Indirect && contribution!=PhotonContribution::Caustic)) return s.fail("Invalid splat parameters");
    const V forward=glm::normalize(target-eye),right=glm::normalize(glm::cross(forward,up)),vertical=glm::cross(right,forward);
    if(paraboloid && (!geometryOnly || width!=height || subdivision<0 || subdivision>6
        || s.paraboloidConfig.vertSpv.empty() || s.paraboloidConfig.fragSpv.empty()))
        return s.fail("Invalid paraboloid settings or missing paraboloid SPIR-V shaders");
    Camera cam{F4(F(eye),0),F4(F(right),0),F4(F(vertical),0),F4(F(forward),0),
        F4(static_cast<float>(std::tan(camera.fovDeg*std::acos(-1.0)/360)),static_cast<float>(width)/height,
            static_cast<float>(farPlane/(farPlane-nearPlane)),static_cast<float>(-nearPlane*farPlane/(farPlane-nearPlane))),
        F4(static_cast<float>(width),static_cast<float>(height),static_cast<float>(radius),static_cast<float>(nearPlane))};
    if(!finite(V(F(cam.eye))) || !finite(V(F(cam.forward)))) return s.fail("Camera outside float range");
    if(paraboloid) cam.projection=F4(static_cast<float>(nearPlane),static_cast<float>(farPlane),0,0);
    std::vector<Triangle> triangles; triangles.reserve(input.size());
    for(const auto& t:input) {
        const V a=vector(t.v0),b=vector(t.v1),c=vector(t.v2),reflectance=vector(t.albedo),normal=glm::cross(b-a,c-a);
        if(!finite(a) || !finite(b) || !finite(c) || !finite(reflectance) || !finite(normal) || glm::length(normal)<1e-12
            || t.metallic!=0 || t.baseColorTextureIndex!=-1 || t.normalTextureIndex!=-1 || t.metallicRoughnessTextureIndex!=-1 || t.emissiveTextureIndex!=-1
            || std::min({reflectance.x,reflectance.y,reflectance.z})<0 || std::max({reflectance.x,reflectance.y,reflectance.z})>1)
            return s.fail("GPU splatting requires finite, diffuse, untextured triangles");
        const Triangle gpu{F4(F(a),0),F4(F(b),0),F4(F(c),0),F4(F(glm::normalize(normal)),0),F4(F(reflectance),0)};
        if(!finite(V(F(gpu.a))) || !finite(V(F(gpu.b))) || !finite(V(F(gpu.c)))) return s.fail("Triangle outside float range");
        triangles.push_back(gpu);
    }
    if(paraboloid) {
        std::vector<Triangle> projected;
        bool overflow=false;
        const std::function<void(V,V,V,const Triangle&,int)> subdivide=[&](V a,V b,V c,const Triangle& original,int level) {
            if(overflow) return;
            if(level>0) {
                const V ab=(a+b)*0.5,bc=(b+c)*0.5,ca=(c+a)*0.5;
                subdivide(a,ab,ca,original,level-1); subdivide(ab,b,bc,original,level-1);
                subdivide(ca,bc,c,original,level-1); subdivide(ab,bc,ca,original,level-1); return;
            }
            if(projected.size()>=1000000) { overflow=true; return; }
            Triangle t=original; t.a=F4(F(a),0); t.b=F4(F(b),0); t.c=F4(F(c),0); projected.push_back(t);
        };
        for(std::size_t id=0;id<triangles.size();++id) {
            auto original=triangles[id]; original.reflectance.w=static_cast<float>(id+1);
            std::vector<V> polygon={V(F(original.a)),V(F(original.b)),V(F(original.c))},clipped;
            // Clip against the forward hemisphere before nonlinear projection.
            V previous=polygon.back(); double previousZ=glm::dot(previous-eye,forward);
            for(const V current:polygon) {
                const double currentZ=glm::dot(current-eye,forward);
                if((currentZ>=0)!=(previousZ>=0)) clipped.push_back(previous+(current-previous)*(previousZ/(previousZ-currentZ)));
                if(currentZ>=0) clipped.push_back(current);
                previous=current; previousZ=currentZ;
            }
            for(std::size_t i=1;i+1<clipped.size();++i) subdivide(clipped[0],clipped[i],clipped[i+1],original,subdivision);
            if(overflow) return s.fail("Paraboloid subdivision exceeds triangle budget");
        }
        if(projected.empty()) { Triangle dummy{}; dummy.a=dummy.b=dummy.c=F4(F(eye-forward),0); projected.push_back(dummy); }
        triangles=std::move(projected);
    }
    std::vector<GpuPhoton> photons;
    for(const auto& p:map.getPhotons()) {
        if(contribution!=PhotonContribution::All && contribution!=p.contribution && !(contribution==PhotonContribution::Indirect && p.contribution==PhotonContribution::Caustic)) continue;
        const GpuPhoton gpu{F4(F(p.position),0),F4(F(p.direction),0),F4(F(p.normal),0),F4(F(p.power),0)};
        if(!finite(V(F(gpu.power)))) return s.fail("Photon power outside float range");
        photons.push_back(gpu);
    }
    if(photons.size()>std::numeric_limits<uint32_t>::max()) return s.fail("Too many photon instances");
    s.stats.splattedPhotons=photons.size(); if(photons.empty()) photons.push_back({});
    const auto& ctx=*s.context; const VkDevice device=ctx.getDevice();
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(ctx.getPhysicalDevice(),&properties);
    if(triangles.size()*sizeof(Triangle)>properties.limits.maxStorageBufferRange || photons.size()*sizeof(GpuPhoton)>properties.limits.maxStorageBufferRange)
        return s.fail("Storage buffer exceeds device limits");
    if(width!=s.width || height!=s.height || paraboloid!=s.paraboloidTarget || !s.geometryPipeline.getPipeline() || !s.splatPipeline.getPipeline()) {
        s.geometryPipeline.destroy(device); s.splatPipeline.destroy(device);
        if(!s.geometry.create(ctx,width,height,true) || !s.accumulation.create(ctx,width,height,false)
            || !s.geometryPipeline.create(ctx,s.geometry.pass,paraboloid?s.paraboloidConfig:s.geometryConfig)
            || !s.splatPipeline.create(ctx,s.accumulation.pass,s.splatConfig)) return s.fail("Render target or pipeline creation failed");
        s.width=width; s.height=height;
        s.paraboloidTarget=paraboloid;
    }
    const VkDeviceSize pixels=static_cast<VkDeviceSize>(width)*height;
    if(!s.cameraBuffer.create(ctx,sizeof(Camera),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)
        || !s.trianglesBuffer.create(ctx,triangles.size()*sizeof(Triangle),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
        || !s.photonsBuffer.create(ctx,photons.size()*sizeof(GpuPhoton),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
        || !s.readback.create(ctx,pixels*36,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true)) return s.fail("Buffer allocation failed");
    std::memcpy(s.cameraBuffer.mapped,&cam,sizeof(cam));
    std::memcpy(s.trianglesBuffer.mapped,triangles.data(),triangles.size()*sizeof(Triangle));
    std::memcpy(s.photonsBuffer.mapped,photons.data(),photons.size()*sizeof(GpuPhoton));
    VkDescriptorBufferInfo buffers[]={{s.cameraBuffer.buffer,0,sizeof(Camera)},
        {s.trianglesBuffer.buffer,0,triangles.size()*sizeof(Triangle)},{s.photonsBuffer.buffer,0,photons.size()*sizeof(GpuPhoton)}};
    VkDescriptorImageInfo images[]={{s.sampler,s.geometry.depthView,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},
        {s.sampler,s.geometry.colorView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[5]{};
    for(uint32_t i=0;i<5;++i) {
        writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=s.descriptor; writes[i].dstBinding=i;
        writes[i].descriptorCount=1; writes[i].descriptorType=i==0?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:(i<3?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        if(i<3) writes[i].pBufferInfo=&buffers[i]; else writes[i].pImageInfo=&images[i-3];
    }
    vkUpdateDescriptorSets(device,5,writes,0,nullptr);
    s.stats.uploadSeconds=elapsed(uploadStart); const auto submitStart=std::chrono::steady_clock::now();
    const VkCommandBuffer cmd=s.pool->beginSingleTimeCommands(); if(!cmd) return s.fail("Command allocation failed");
    if(s.queries) { vkCmdResetQueryPool(cmd,s.queries,0,4); vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,s.queries,0); }
    const auto draw=[&](const Target& target,const VKG::VulkanPipeline& pipeline,bool geometry) {
        VkClearValue clear[2]{}; clear[1].depthStencil={1,0};
        VkRenderPassBeginInfo begin{}; begin.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass=target.pass; begin.framebuffer=target.framebuffer; begin.renderArea={{0,0},{static_cast<uint32_t>(width),static_cast<uint32_t>(height)}};
        begin.clearValueCount=geometry?2:1; begin.pClearValues=clear;
        vkCmdBeginRenderPass(cmd,&begin,VK_SUBPASS_CONTENTS_INLINE);
        const VkViewport viewport{0,0,static_cast<float>(width),static_cast<float>(height),0,1};
        const VkRect2D scissor{{0,0},{static_cast<uint32_t>(width),static_cast<uint32_t>(height)}};
        vkCmdSetViewport(cmd,0,1,&viewport); vkCmdSetScissor(cmd,0,1,&scissor);
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.getPipeline());
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline.getLayout(),0,1,&s.descriptor,0,nullptr);
        if(geometry) vkCmdDraw(cmd,static_cast<uint32_t>(triangles.size()*3),1,0,0);
        else if(s.stats.splattedPhotons) vkCmdDraw(cmd,6,static_cast<uint32_t>(s.stats.splattedPhotons),0,0);
        vkCmdEndRenderPass(cmd);
    };
    draw(s.geometry,s.geometryPipeline,true);
    if(s.queries) vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.queries,1);
    if(!geometryOnly) draw(s.accumulation,s.splatPipeline,false);
    if(s.queries) vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.queries,2);
    VkImageMemoryBarrier barriers[2]{};
    for(int i=0;i<2;++i) {
        auto& b=barriers[i]; b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|(i?VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT:VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        b.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout=i?VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.image=i?s.geometry.depth:s.geometry.color;
        b.subresourceRange={static_cast<VkImageAspectFlags>(i?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
    }
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,2,barriers);
    const VkImage copies[]={s.accumulation.color,s.geometry.color,s.geometry.depth};
    for(int i=0;i<3;++i) {
        if(geometryOnly && i==0) continue;
        VkBufferImageCopy copy{}; copy.bufferOffset=pixels*16*i;
        copy.imageSubresource={static_cast<VkImageAspectFlags>(i==2?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT),0,0,1};
        copy.imageExtent={static_cast<uint32_t>(width),static_cast<uint32_t>(height),1};
        vkCmdCopyImageToBuffer(cmd,copies[i],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,s.readback.buffer,1,&copy);
    }
    VkMemoryBarrier host{}; host.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
    if(s.queries) vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.queries,3);
    if(!s.pool->endSingleTimeCommands(cmd)) return s.fail("Submission failed");
    s.stats.submitReadbackSeconds=elapsed(submitStart);
    if(s.queries) {
        uint64_t stamps[4]{};
        if(vkGetQueryPoolResults(device,s.queries,0,4,sizeof(stamps),stamps,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT)==VK_SUCCESS) {
            const double scale=ctx.getTimestampPeriodNs()*1e-6;
            s.stats.gbufferGpuMilliseconds=(stamps[1]-stamps[0])*scale;
            s.stats.splatGpuMilliseconds=(stamps[2]-stamps[1])*scale;
            s.stats.copyGpuMilliseconds=(stamps[3]-stamps[2])*scale;
        }
    }
    const auto* colors=static_cast<const F4*>(s.readback.mapped);
    const auto* normals=colors+pixels;
    const auto* depths=reinterpret_cast<const float*>(colors+pixels*2);
    Graphics::Imagef image(width,height); PhotonGBuffer gbuffer{width,height,{}};
    gbuffer.receivers.resize(static_cast<std::size_t>(pixels));
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
        const std::size_t i=static_cast<std::size_t>(y)*width+x;
        if(!geometryOnly && (!finite(V(F(colors[i]))) || std::min({colors[i].x,colors[i].y,colors[i].z})<0)) return s.fail("Invalid GPU radiance");
        image.setColor(x,y,geometryOnly?Graphics::ColorRGBAf(0,0,0,1):Graphics::ColorRGBAf(colors[i].x,colors[i].y,colors[i].z,1));
        auto& receiver=gbuffer.receivers[i]; receiver.depth=depths[i];
        const int id=static_cast<int>(normals[i].w)-1;
        if(id<0) continue;
        if(id>=static_cast<int>(input.size()) || !std::isfinite(depths[i])) return s.fail("Invalid GPU receiver");
        const float z=paraboloid?0:cam.projection.w/(depths[i]-cam.projection.z);
        const float u=2*(x+0.5f)/width-1,v=2*(y+0.5f)/height-1;
        F position;
        if(paraboloid) {
            const float r2=u*u+v*v;
            if(r2>1) continue;
            const float distance=cam.projection.x+depths[i]*(cam.projection.y-cam.projection.x);
            const F direction=(F(cam.right)*(2*u)-F(cam.up)*(2*v)+F(cam.forward)*(1-r2))/(1+r2);
            position=F(cam.eye)+direction*distance;
        } else position=F(cam.eye)+F(cam.forward)*z+F(cam.right)*(u*z*cam.projection.x*cam.projection.y)-F(cam.up)*(v*z*cam.projection.x);
        if(!finite(V(position))) return s.fail("Depth reconstruction outside finite range");
        const F normal=glm::normalize(F(normals[i]));
        if(glm::dot(normal,F(cam.eye)-position)<=0) continue;
        receiver.triangle=id; receiver.position=V(position); receiver.normal=V(normal);
    }
    indirect=std::move(image); receivers=std::move(gbuffer); return true;
}
} // namespace Phantom::RayTracer
