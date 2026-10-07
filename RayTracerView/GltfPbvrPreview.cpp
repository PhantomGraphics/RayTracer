#include "pch.h"
#include "GltfPbvrPreview.h"
#include "../PhotonSplatGpu/DepthPhotonTransport.h"
#include "../../CGLib/VulkanGraphics/VulkanContext.h"
#include "../../CGLib/VulkanGraphics/VulkanCommandPool.h"
#include "../../CGLib/VulkanGraphics/VulkanSPVResolver.h"
#include <limits>

namespace RT=Phantom::RayTracer;
using V=Phantom::Math::Vector3dd;
struct GltfPbvrPreview::Runtime {
    Phantom::VKG::VulkanContext context;
    Phantom::VKG::VulkanCommandPool pool;
    RT::PhotonSplatGpu gpu;
    bool ready=false;
    std::uint64_t directGeneration=0;
    Phantom::Graphics::Imagef direct;
    ~Runtime() { gpu.destroy(); pool.destroy(); }
    bool create() {
        if(!context.createInstance("glTF PBVR preview",{},true))
            if(!context.createInstance("glTF PBVR preview",{},false)) return false;
        ready=context.initDevice(VK_NULL_HANDLE)&&pool.init(&context,VK_NULL_HANDLE)
            &&gpu.create(context,pool,(Phantom::VKG::detail::detectModuleDir()/"photon_splat_shaders").string());
        return ready;
    }
};
GltfPbvrPreview::GltfPbvrPreview()=default;
GltfPbvrPreview::~GltfPbvrPreview() { cleanup(); }
void GltfPbvrPreview::cleanup() {
    if(future_.valid()) { future_.wait(); future_.get(); }
    runtime_.reset();
}
void GltfPbvrPreview::invalidate() {
    ++generation_; samples_=0; coarse_=true; dirty_=true;
    mean_.clear(); direct_.clear(); positions_.clear(); normals_.clear();
    changed_=std::chrono::steady_clock::now();
}
void GltfPbvrPreview::setScene(const Phantom::Gltf::GltfDocument& document) {
    invalidate(); haveCamera_=false; scene_.reset();
    auto built=std::make_shared<GltfBuildResult>(buildFromGltf(document));
    if(built->triangles.empty()) { status_="No glTF geometry"; return; }
    if(!document.skins.empty() || !document.animations.empty() || !document.textures.empty()) {
        status_="PBVR: static untextured glTF only (raster preview remains available)"; return;
    }
    if(!built->lights.empty()) { status_="PBVR: emissive area lights only; punctual lights are not transported"; return; }
    for(const auto& mesh:document.meshes) for(const auto& primitive:mesh.primitives) {
        if(primitive.colorAccessor>=0 || !primitive.targets.empty()) {
            status_="PBVR: vertex colors and morph targets are not transported"; return;
        }
    }
    bool emissive=false;
    V lo(std::numeric_limits<double>::max()),hi(-std::numeric_limits<double>::max());
    for(const auto& t:built->triangles) {
        if(t.metallic!=0 || t.baseColorTextureIndex!=-1 || t.normalTextureIndex!=-1
            || t.metallicRoughnessTextureIndex!=-1 || t.emissiveTextureIndex!=-1) {
            status_="PBVR: diffuse untextured materials only"; return;
        }
        for(int c=0;c<3;++c) {
            emissive|=t.emission[c]>0;
            lo[c]=std::min({lo[c],t.v0[c],t.v1[c],t.v2[c]});
            hi[c]=std::max({hi[c],t.v0[c],t.v1[c],t.v2[c]});
        }
    }
    // Alpha modes, double-sided and material extensions require transport support.
    for(const auto& m:document.materials) {
        if(m.alphaMode!=Phantom::Gltf::GltfAlphaMode::Opaque || m.doubleSided) {
            status_="PBVR: opaque one-sided materials only"; return;
        }
    }
    if(!emissive) { status_="PBVR needs an emissive glTF area light"; return; }
    radius_=std::max(1e-5,glm::length(hi-lo)*0.035);
    scene_=std::move(built); status_="PBVR ready";
}
void GltfPbvrPreview::launch() {
    const auto scene=scene_; const auto generation=generation_; const bool coarse=coarse_;
    const int sample=samples_,width=width_,height=height_; const double radius=radius_;
    RT::RtCameraSpec camera;
    for(int c=0;c<3;++c) { camera.lookFrom[c]=camera_.eye[c]; camera.lookAt[c]=camera_.target[c]; camera.up[c]=camera_.up[c]; }
    camera.fovDeg=camera_.fovDeg;
    future_=std::async(std::launch::async,[this,scene,generation,coarse,sample,width,height,radius,camera] {
        Result result; result.generation=generation; result.coarse=coarse;
        const auto start=std::chrono::steady_clock::now();
        if(!runtime_) runtime_=std::make_unique<Runtime>();
        auto& r=*runtime_;
        if(!r.ready && !r.create()) { result.error="PBVR Vulkan initialization failed"; return result; }
        RT::DepthPhotonSettings settings; settings.projection=RT::DepthPhotonProjection::Paraboloid;
        settings.firstResolution=coarse?16:64; settings.minResolution=coarse?8:16;
        settings.lightSamples=coarse?2:8; settings.maxDepth=coarse?3:4;
        settings.selection.maxParticles=coarse?8:32; settings.paraboloidSubdivision=coarse?2:3;
        settings.sourceOffset=radius/1200; settings.nearPlane=radius/120;
        settings.farPlane=std::max(1000.0,radius*1000);
        settings.randomSeed=42u+static_cast<std::uint32_t>(sample)*0x9e3779b9u;
        settings.selection.randomSeed=settings.randomSeed^0x68bc21ebu;
        RT::DepthPhotonTransport transport; RT::PhotonMap photons;
        if(!transport.build(r.gpu,scene->triangles,settings,photons)) { result.error=transport.getLastError(); return result; }
        RT::PhotonGBuffer receivers; Phantom::Graphics::Imagef indirect;
        if(!r.gpu.render(scene->triangles,photons,camera,width,height,radius,indirect,receivers,
            RT::PhotonContribution::Indirect,0.001,1000)) { result.error=r.gpu.getLastError(); return result; }
        if(r.directGeneration!=generation) {
            RT::PhotonMappingSettings shading; shading.buildPhotonIndex=false;
            shading.directLightSamples=256; shading.render.randomSeed=42;
            RT::PhotonMapper mapper(shading); Phantom::Graphics::Imagef zero(width,height);
            for(int y=0;y<height;++y) for(int x=0;x<width;++x) zero.setColor(x,y,{0,0,0,1});
            if(!mapper.buildWithPhotons(scene->triangles,{}) || !mapper.shadeGBuffer(receivers,r.direct,&zero)) {
                result.error="PBVR area-light shading failed"; return result;
            }
            r.directGeneration=generation;
        }
        result.lighting.resize(width*height); result.direct.resize(width*height); result.positions.resize(width*height); result.normals.resize(width*height);
        for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
            const int i=y*width+x; const auto a=indirect.getColor(x,y),d=r.direct.getColor(x,y);
            const auto& receiver=receivers.receivers[i];
            result.lighting[i]=glm::vec4(a.x,a.y,a.z,receiver.triangle>=0?1.f:0.f);
            result.direct[i]=glm::vec4(d.x,d.y,d.z,0);
            result.positions[i]=glm::vec4(glm::vec3(receiver.position),0);
            result.normals[i]=glm::vec4(glm::vec3(receiver.normal),0);
        }
        result.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(); return result;
    });
}
void GltfPbvrPreview::update(Phantom::Gltf::GltfSceneRenderer& renderer) {
    const auto camera=renderer.getCameraParams(); const auto extent=renderer.getExtent();
    if(!haveCamera_ || camera.eye!=camera_.eye || camera.target!=camera_.target || camera.up!=camera_.up
        || camera.fovDeg!=camera_.fovDeg || extent.width!=extent_.width || extent.height!=extent_.height) {
        camera_=camera; extent_=extent; haveCamera_=true; invalidate();
        const double divisor=std::max(8.0,std::max(extent.width,extent.height)/256.0);
        width_=std::max(1,static_cast<int>(extent.width/divisor)); height_=std::max(1,static_cast<int>(extent.height/divisor));
    }
    if(future_.valid() && future_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        auto result=future_.get();
        if(result.generation==generation_ && enabled_ && scene_) {
            if(!result.error.empty()) { status_=result.error; scene_.reset(); dirty_=true; }
            else {
                if(mean_.empty()) mean_.assign(result.lighting.size(),glm::vec4(0));
                ++samples_;
                for(std::size_t i=0;i<mean_.size();++i) mean_[i]+=(result.lighting[i]-mean_[i])/static_cast<float>(samples_);
                positions_=std::move(result.positions); normals_=std::move(result.normals);
                direct_=std::move(result.direct);
                lastSeconds_=result.seconds; dirty_=true;
            }
        }
    }
    if(dirty_) {
        std::vector<glm::vec4> field(2,glm::vec4(0));
        if(enabled_ && scene_ && !mean_.empty()) {
            const float aspectCorrection=extent_.height?static_cast<float>(extent_.width)*height_/(extent_.height*width_):1.f;
            field[0]=glm::vec4(width_,height_,radius_,aspectCorrection);
            field[1]=glm::vec4(indirectGain_,0,0,0);
            field.insert(field.end(),mean_.begin(),mean_.end());
            field.insert(field.end(),positions_.begin(),positions_.end());
            field.insert(field.end(),normals_.begin(),normals_.end());
            field.insert(field.end(),direct_.begin(),direct_.end());
        }
        renderer.setScalarField(std::move(field)); dirty_=false;
    }
    if(enabled_ && scene_ && coarse_ && samples_>0 && std::chrono::duration<double>(std::chrono::steady_clock::now()-changed_).count()>=0.35) {
        ++generation_; coarse_=false; samples_=0; mean_.clear(); // Keep last coarse lighting until refinement arrives.
    }
    if(enabled_ && scene_ && !future_.valid() && (!coarse_ || samples_==0)) launch();
}
void GltfPbvrPreview::drawControls() {
    if(ImGui::Checkbox("PBVR glTF lighting",&enabled_)) invalidate();
    if(ImGui::SliderFloat("Indirect gain",&indirectGain_,0,2)) dirty_=true;
    ImGui::TextWrapped("%s",status_.c_str());
    if(scene_) ImGui::Text("%s: %d passes / %.1f ms / %dx%d",coarse_?"Preview":"Refining",samples_,lastSeconds_*1000,width_,height_);
}
