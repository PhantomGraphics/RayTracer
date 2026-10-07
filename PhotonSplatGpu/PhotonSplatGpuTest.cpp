#include "PhotonSplatGpu.h"
#include "DepthPhotonTransport.h"
#include "../../CGLib/VulkanGraphics/VulkanContext.h"
#include "../../CGLib/VulkanGraphics/VulkanCommandPool.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cmath>
#include <limits>

namespace {
using namespace Phantom::RayTracer;
using V=Phantom::Math::Vector3dd;
std::vector<RtTriangle> plane(double z,bool backside=false)
{
    const V points[]={V(-2,-2,z),V(-2,2,z),V(2,2,z),V(2,-2,z)};
    std::vector<RtTriangle> result;
    for(auto ids: {std::vector<int>{0,1,2},std::vector<int>{0,2,3}}) {
        if(backside) std::swap(ids[1],ids[2]); RtTriangle t;
        for(int c=0;c<3;++c) { t.v0[c]=points[ids[0]][c]; t.v1[c]=points[ids[1]][c]; t.v2[c]=points[ids[2]][c]; }
        t.albedo[0]=0.5; t.albedo[1]=0.25; t.albedo[2]=0.125; result.push_back(t);
    }
    return result;
}
RtCameraSpec camera()
{
    RtCameraSpec c;
    c.lookFrom[0]=c.lookFrom[1]=c.lookFrom[2]=0;
    c.lookAt[0]=c.lookAt[1]=0; c.lookAt[2]=1; return c;
}
Photon photon(V position=V(0,0,2))
{
    Photon p; p.position=position; p.normal=V(0,0,-1); p.direction=V(0,0,1);
    p.power=V(1,2,3); p.contribution=PhotonContribution::Indirect; return p;
}
class PhotonSplatGpuTest : public testing::Test {
protected:
    Phantom::VKG::VulkanContext context;
    Phantom::VKG::VulkanCommandPool pool;
    PhotonSplatGpu gpu;
    VkDebugUtilsMessengerEXT messenger=VK_NULL_HANDLE;
    std::atomic<int> errors{0};
    static VKAPI_ATTR VkBool32 VKAPI_CALL callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT*,void* user) {
        if(severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++*static_cast<std::atomic<int>*>(user);
        return VK_FALSE;
    }
    void SetUp() override {
        if(!context.createInstance("Photon splat tests",{},true)) {
            if(!context.createInstance("Photon splat tests",{},false)) GTEST_SKIP()<<"No Vulkan instance";
        }
        if(!context.initDevice(VK_NULL_HANDLE)) GTEST_SKIP()<<"No Vulkan device";
        ASSERT_TRUE(pool.init(&context,VK_NULL_HANDLE));
        if(context.isValidationEnabled()) {
            VkDebugUtilsMessengerCreateInfoEXT ci{}; ci.sType=VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
            ci.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            ci.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
            ci.pfnUserCallback=callback; ci.pUserData=&errors;
            const auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(context.getInstance(),"vkCreateDebugUtilsMessengerEXT"));
            ASSERT_NE(create,nullptr); ASSERT_EQ(create(context.getInstance(),&ci,nullptr,&messenger),VK_SUCCESS);
        }
        ASSERT_TRUE(gpu.create(context,pool,PHOTON_SPLAT_TEST_SHADERS))<<gpu.getLastError();
    }
    void TearDown() override {
        gpu.destroy(); pool.destroy(); EXPECT_EQ(errors.load(),0);
        if(messenger) {
            const auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(context.getInstance(),"vkDestroyDebugUtilsMessengerEXT"));
            if(destroy) destroy(context.getInstance(),messenger,nullptr);
        }
    }
    void compare(const std::vector<RtTriangle>& scene,const PhotonMap& map,double radius,int width=16,int height=16,
                 PhotonContribution contribution=PhotonContribution::Indirect) {
        Phantom::Graphics::Imagef image; PhotonGBuffer gbuffer;
        ASSERT_TRUE(gpu.render(scene,map,camera(),width,height,radius,image,gbuffer,contribution))<<gpu.getLastError();
        ASSERT_EQ(gbuffer.receivers.size(),static_cast<std::size_t>(width*height));
        for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
            const auto& p=gbuffer.receivers[y*width+x]; V expected(0);
            if(p.triangle>=0) {
                const auto& t=scene[p.triangle];
                expected=map.estimateRadiance(p.position,p.normal,V(t.albedo[0],t.albedo[1],t.albedo[2]),radius,contribution);
            }
            const auto actual=image.getColor(x,y);
            for(int c=0;c<3;++c) EXPECT_NEAR(actual[c],expected[c],1e-6)<<x<<','<<y<<" channel="<<c;
        }
    }
};
}
TEST_F(PhotonSplatGpuTest, AddsOverlappingPhotonsAndMatchesKdTree)
{
    PhotonMap map; ASSERT_TRUE(map.build({photon(),photon()})); compare(plane(2),map,1);
    EXPECT_EQ(gpu.getStats().splattedPhotons,2u);
}
TEST_F(PhotonSplatGpuTest, AcceptsUnindexedPhotonStorage)
{
    PhotonMap map; ASSERT_TRUE(map.build({photon(),photon(V(0.2,0,2))},false));
    EXPECT_FALSE(map.hasSpatialIndex()); compare(plane(2),map,1);
}
TEST_F(PhotonSplatGpuTest, DepthSelectsFrontSurfaceAndRejectsOtherPlane)
{
    auto scene=plane(2); const auto rear=plane(2.3); scene.insert(scene.end(),rear.begin(),rear.end());
    PhotonMap map; ASSERT_TRUE(map.build({photon(V(0,0,2.3))})); compare(scene,map,1);
    Phantom::Graphics::Imagef image; PhotonGBuffer gbuffer;
    ASSERT_TRUE(gpu.render(scene,map,camera(),16,16,1,image,gbuffer));
    for(const auto& p:gbuffer.receivers) { EXPECT_GE(p.triangle,0); EXPECT_LT(p.triangle,2); EXPECT_NEAR(p.position.z,2,1e-4); }
    for(int y=0;y<16;++y) for(int x=0;x<16;++x) EXPECT_FLOAT_EQ(image.getColor(x,y).x,0);
}
TEST_F(PhotonSplatGpuTest, ConservativeBoundsIncludeOffscreenAndNearPlaneSplats)
{
    PhotonMap map; ASSERT_TRUE(map.build({photon(V(0.9,0,2))})); compare(plane(2),map,1.2);
    ASSERT_TRUE(map.build({photon()})); compare(plane(2),map,3);
}
TEST_F(PhotonSplatGpuTest, EmptyMapAndResize)
{
    PhotonMap map; ASSERT_TRUE(map.build({})); compare(plane(2),map,1,8,12);
    EXPECT_EQ(gpu.getStats().splattedPhotons,0u);
    ASSERT_TRUE(map.build({photon()})); compare(plane(2),map,1,24,8);
}
TEST_F(PhotonSplatGpuTest, ContributionAndBackFaceFiltering)
{
    auto direct=photon(); direct.contribution=PhotonContribution::Direct;
    auto caustic=photon(); caustic.contribution=PhotonContribution::Caustic;
    PhotonMap map; ASSERT_TRUE(map.build({direct,caustic})); compare(plane(2),map,1);
    EXPECT_EQ(gpu.getStats().splattedPhotons,1u);
    compare(plane(2),map,1,16,16,PhotonContribution::All);
    EXPECT_EQ(gpu.getStats().splattedPhotons,2u);
    compare(plane(2),map,1,16,16,PhotonContribution::Direct);
    compare(plane(2),map,1,16,16,PhotonContribution::Caustic);
    compare(plane(2,true),map,1);
}
TEST_F(PhotonSplatGpuTest, TiltedSurfaceAndNearClipping)
{
    auto scene=plane(2);
    for(auto& t:scene) { t.v0[2]+=0.2*t.v0[0]; t.v1[2]+=0.2*t.v1[0]; t.v2[2]+=0.2*t.v2[0]; }
    auto p=photon(); p.normal=glm::normalize(V(0.2,0,-1)); p.direction=-p.normal;
    PhotonMap map; ASSERT_TRUE(map.build({p})); compare(scene,map,1.5);
    compare(plane(0.005),map,1.5);
}
TEST_F(PhotonSplatGpuTest, RejectsInvalidInputsWithoutChangingOutputs)
{
    auto scene=plane(2); scene[0].metallic=1; PhotonMap map; ASSERT_TRUE(map.build({photon()}));
    Phantom::Graphics::Imagef image(2,2); PhotonGBuffer gbuffer;
    EXPECT_FALSE(gpu.render(scene,map,camera(),16,16,1,image,gbuffer));
    EXPECT_EQ(image.getWidth(),2); EXPECT_EQ(gbuffer.width,0);
    EXPECT_FALSE(gpu.render(plane(2),map,camera(),16,16,0,image,gbuffer));
}
TEST_F(PhotonSplatGpuTest, DepthRasterChoosesNearestOccluder)
{
    auto scene=plane(2); const auto far=plane(3); scene.insert(scene.end(),far.begin(),far.end());
    PhotonGBuffer buffer;
    ASSERT_TRUE(gpu.rasterize(scene,camera(),8,8,buffer,0.01,10))<<gpu.getLastError();
    for(const auto& p:buffer.receivers) {
        ASSERT_GE(p.triangle,0); EXPECT_LT(p.triangle,2);
        EXPECT_NEAR(p.position.z,2,1e-4);
    }
    PhotonMap empty; Phantom::Graphics::Imagef image;
    // Switching from depth-only to full splat must keep resources/layouts valid.
    ASSERT_TRUE(gpu.render(scene,empty,camera(),8,8,1,image,buffer));
}
TEST_F(PhotonSplatGpuTest, DepthTransportFluxBudgetResolutionAndRepeatability)
{
    auto scene=plane(2); auto light=plane(0,true);
    for(auto& t:light) for(int c=0;c<3;++c) t.emission[c]=c+1;
    scene.insert(scene.end(),light.begin(),light.end());
    DepthPhotonSettings cfg; cfg.lightSamples=1; cfg.firstResolution=8; cfg.minResolution=4;
    cfg.maxDepth=3; cfg.selection.maxParticles=2; cfg.storeDirectPhotons=true;
    DepthPhotonTransport transport; PhotonMap a,b;
    ASSERT_TRUE(transport.build(gpu,scene,cfg,a))<<transport.getLastError();
    ASSERT_FALSE(a.getPhotons().empty()); EXPECT_FALSE(a.hasSpatialIndex());
    const auto stats=transport.getStats(); ASSERT_EQ(stats.bounces.size(),3u);
    for(std::size_t i=0;i<stats.bounces.size();++i) {
        const auto& s=stats.bounces[i]; EXPECT_EQ(s.depthMaps,5*s.sources);
        EXPECT_EQ(s.resolution,i==0?8:4); EXPECT_LE(s.retained,2u);
        for(int c=0;c<3;++c) EXPECT_NEAR(s.launchedFlux[c],s.arrivedFlux[c]+s.escapedFlux[c],1e-9);
    }
    ASSERT_TRUE(transport.build(gpu,scene,cfg,b))<<transport.getLastError();
    ASSERT_EQ(a.getPhotons().size(),b.getPhotons().size());
    for(std::size_t i=0;i<a.getPhotons().size();++i) {
        const auto& p=a.getPhotons()[i]; const auto& q=b.getPhotons()[i];
        EXPECT_EQ(p.bounce,q.bounce);
        for(int c=0;c<3;++c) { EXPECT_DOUBLE_EQ(p.power[c],q.power[c]); EXPECT_DOUBLE_EQ(p.position[c],q.position[c]); }
    }
    cfg.maxStoredPhotons=1;
    EXPECT_FALSE(transport.build(gpu,scene,cfg,b)); EXPECT_TRUE(b.getPhotons().empty());
    EXPECT_TRUE(transport.getStats().bounces.empty());
}
TEST_F(PhotonSplatGpuTest, DepthTransportRejectsUnsupportedInputs)
{
    auto scene=plane(2); for(auto& t:scene) t.emission[0]=1;
    DepthPhotonTransport transport; DepthPhotonSettings cfg; PhotonMap output;
    cfg.firstResolution=0; EXPECT_FALSE(transport.build(gpu,scene,cfg,output));
    cfg.firstResolution=32; scene[0].metallic=1;
    EXPECT_FALSE(transport.build(gpu,scene,cfg,output));
    scene[0].metallic=0; cfg.selection.selection=PbvrSelection::SpatialPower;
    EXPECT_FALSE(transport.build(gpu,scene,cfg,output));
    cfg.selection.spatialCellSize=0.1; cfg.sourceOffset=std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(transport.build(gpu,scene,cfg,output));
}
