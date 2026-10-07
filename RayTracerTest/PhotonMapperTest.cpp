#include "pch.h"
#include "../RayTracer/PhotonMapper.h"
#include <cmath>
#include <limits>

namespace {
using namespace Phantom::RayTracer;
using V=Phantom::Math::Vector3dd;
void quad(std::vector<RtTriangle>& scene,V origin,V u,V v,V color,V emission=V(0))
{
    const V vertices[]={origin,origin+u,origin+u+v,origin+v};
    for(const auto& indices: {std::vector<int>{0,1,2},std::vector<int>{0,2,3}}) {
        RtTriangle t; t.roughness=1;
        for(int c=0;c<3;++c) { t.v0[c]=vertices[indices[0]][c]; t.v1[c]=vertices[indices[1]][c];
            t.v2[c]=vertices[indices[2]][c]; t.albedo[c]=color[c]; t.emission[c]=emission[c]; }
        scene.push_back(t);
    }
}
std::vector<RtTriangle> enclosure(V color=V(0.7))
{
    std::vector<RtTriangle> s;
    quad(s,V(0,0,0),V(0,2,0),V(0,0,2),color); // +x
    quad(s,V(2,0,0),V(0,0,2),V(0,2,0),color); // -x
    quad(s,V(0,0,0),V(0,0,2),V(2,0,0),color); // +y
    quad(s,V(0,2,0),V(2,0,0),V(0,0,2),color); // -y
    quad(s,V(0,0,2),V(0,2,0),V(2,0,0),color); // -z
    quad(s,V(0.7,1.999,0.7),V(0.6,0,0),V(0,0,0.6),V(0),V(4));
    return s;
}
PhotonMappingSettings settings()
{
    PhotonMappingSettings s; s.photonCount=4000; s.photonMaxDepth=4; s.gatherRadius=0.3;
    s.render.width=12; s.render.height=12; s.render.samplesPerPixel=2;
    return s;
}
RtCameraSpec camera()
{
    RtCameraSpec c; c.lookFrom[0]=1; c.lookFrom[1]=1; c.lookFrom[2]=-3.2;
    c.lookAt[0]=1; c.lookAt[1]=1; c.lookAt[2]=1; return c;
}
}

TEST(PhotonMapTest, FluxNormalizationAndNoExtraCosine)
{
    Photon p; p.power=V(2,4,6); p.direction=glm::normalize(V(1,-1,0));
    PhotonMap map; ASSERT_TRUE(map.build({p}));
    const V value=map.estimateRadiance(V(0),V(0,1,0),V(0.5),2);
    const double pi=std::acos(-1.0);
    EXPECT_NEAR(value.x,1/(4*pi*pi),1e-12);
    EXPECT_NEAR(value.z,3/(4*pi*pi),1e-12);
}
TEST(PhotonMapTest, SurfaceAndContributionFiltering)
{
    Photon direct; direct.power=V(1);
    Photon indirect=direct; indirect.contribution=PhotonContribution::Indirect;
    Photon caustic=direct; caustic.contribution=PhotonContribution::Caustic;
    Photon opposite=indirect; opposite.normal=V(0,-1,0);
    Photon parallel=indirect; parallel.position=V(0,0.1,0);
    Photon outside=indirect; outside.position=V(2,0,0);
    PhotonMap map; ASSERT_TRUE(map.build({direct,indirect,caustic,opposite,parallel,outside}));
    const double unit=1/(std::acos(-1.0)*std::acos(-1.0));
    EXPECT_NEAR(map.estimateRadiance(V(0),V(0,1,0),V(1),1,PhotonContribution::Indirect).x,2*unit,1e-12);
    EXPECT_NEAR(map.estimateRadiance(V(0),V(0,1,0),V(1),1,PhotonContribution::Direct).x,unit,1e-12);
    EXPECT_NEAR(map.estimateRadiance(V(0),V(0,1,0),V(1),1,PhotonContribution::Caustic).x,unit,1e-12);
}
TEST(PhotonMapTest, RebuildAndInvalidInput)
{
    Photon p; p.power=V(1); PhotonMap map; ASSERT_TRUE(map.build({p}));
    ASSERT_TRUE(map.build(map.getPhotons())); EXPECT_EQ(map.getPhotons().size(),1u);
    p.power.x=std::numeric_limits<double>::quiet_NaN(); EXPECT_FALSE(map.build({p}));
    EXPECT_TRUE(map.getPhotons().empty());
    EXPECT_EQ(map.estimateRadiance(V(0),V(0,1,0),V(1),0),V(0));
}
TEST(PhotonMapperTest, EmissionPowerAndBlackAbsorption)
{
    auto cfg=settings(); cfg.photonMaxDepth=4; PhotonMapper mapper(cfg);
    ASSERT_TRUE(mapper.build(enclosure(V(0)))) << mapper.getLastError();
    EXPECT_EQ(mapper.getStats().emittedPhotons,cfg.photonCount);
    EXPECT_GT(mapper.getStats().directPhotons,1000u);
    EXPECT_EQ(mapper.getStats().indirectPhotons,0u);
    const double power=4*0.36*std::acos(-1.0)/cfg.photonCount;
    for(const auto& p:mapper.getPhotonMap().getPhotons()) EXPECT_NEAR(p.power.x,power,1e-12);
}
TEST(PhotonMapperTest, DeterministicIndirectTransport)
{
    PhotonMapper a(settings()),b(settings());
    ASSERT_TRUE(a.build(enclosure())); ASSERT_TRUE(b.build(enclosure()));
    EXPECT_GT(a.getStats().indirectPhotons,0u);
    const auto& pa=a.getPhotonMap().getPhotons(); const auto& pb=b.getPhotonMap().getPhotons();
    ASSERT_EQ(pa.size(),pb.size());
    for(std::size_t i=0;i<pa.size();++i) { EXPECT_EQ(pa[i].position,pb[i].position); EXPECT_EQ(pa[i].power,pb[i].power); }
    Phantom::Graphics::Imagef image; ASSERT_TRUE(a.renderLinear(camera(),image));
    EXPECT_EQ(image.getWidth(),12); double sum=0;
    for(float value:image.getValues()) EXPECT_TRUE(std::isfinite(value));
    for(int y=0;y<12;++y) for(int x=0;x<12;++x) sum+=image.getColor(x,y).x;
    EXPECT_GT(sum,0);
}
TEST(PhotonMapperTest, MultipleEmittersRespectSelectionProbability)
{
    auto scene=enclosure(V(0));
    // Double one half of the light: sampled photon powers remain equal in gray scenes.
    for(int c=0;c<3;++c) scene.back().emission[c]*=2;
    PhotonMapper mapper(settings()); ASSERT_TRUE(mapper.build(scene));
    const double expected=4*0.54*std::acos(-1.0)/settings().photonCount;
    for(const auto& p:mapper.getPhotonMap().getPhotons()) EXPECT_NEAR(p.power.x,expected,1e-12);
}
TEST(PhotonMapperTest, DirectLightIsNotCountedTwice)
{
    auto a=settings(); a.photonMaxDepth=1;
    auto b=a; b.photonCount*=2;
    PhotonMapper first(a),second(b);
    ASSERT_TRUE(first.build(enclosure())); ASSERT_TRUE(second.build(enclosure()));
    ASSERT_EQ(first.getStats().indirectPhotons,0u);
    Phantom::Graphics::Imagef left,right;
    ASSERT_TRUE(first.renderLinear(camera(),left)); ASSERT_TRUE(second.renderLinear(camera(),right));
    EXPECT_EQ(left.getValues(),right.getValues());
}
TEST(PhotonMapperTest, RussianRoulettePreservesColoredFlux)
{
    auto cfg=settings(); cfg.photonMaxDepth=2;
    PhotonMapper mapper(cfg); ASSERT_TRUE(mapper.build(enclosure(V(0.5,0.25,0.125))));
    ASSERT_GT(mapper.getStats().indirectPhotons,0u);
    const double initial=4*0.36*std::acos(-1.0)/cfg.photonCount;
    for(const auto& p:mapper.getPhotonMap().getPhotons()) if(p.contribution==PhotonContribution::Indirect) {
        EXPECT_NEAR(p.power.x,initial,1e-12);
        EXPECT_NEAR(p.power.y,initial*0.5,1e-12);
        EXPECT_NEAR(p.power.z,initial*0.25,1e-12);
    }
}
TEST(PhotonMapperTest, MirrorPathsProduceCausticPhotons)
{
    auto scene=enclosure();
    // The floor is an ideal mirror; reflected rays can hit diffuse walls.
    for(int i=4;i<6;++i) { scene[i].metallic=1; scene[i].roughness=0; }
    PhotonMapper mapper(settings()); ASSERT_TRUE(mapper.build(scene));
    EXPECT_GT(mapper.getStats().causticPhotons,0u);
}
TEST(PhotonMapperTest, RejectsUnsupportedMaterialsAndClearsOldMap)
{
    PhotonMapper mapper(settings()); auto scene=enclosure(); ASSERT_TRUE(mapper.build(scene));
    scene[0].baseColorTextureIndex=0;
    EXPECT_FALSE(mapper.build(scene)); EXPECT_TRUE(mapper.getPhotonMap().getPhotons().empty());
    Phantom::Graphics::Imagef image; EXPECT_FALSE(mapper.renderLinear(camera(),image));
    scene=enclosure(); scene[0].metallic=0.5; EXPECT_FALSE(mapper.build(scene));
    scene=enclosure(); scene[0].v1[0]=scene[0].v0[0]; scene[0].v1[1]=scene[0].v0[1]; scene[0].v1[2]=scene[0].v0[2];
    EXPECT_FALSE(mapper.build(scene));
}
TEST(PhotonMapperTest, ValidatesCameraSettingsAndLight)
{
    auto cfg=settings(); cfg.photonCount=0; PhotonMapper invalid(cfg);
    EXPECT_FALSE(invalid.build(enclosure()));
    PhotonMapper mapper(settings()); auto scene=enclosure();
    for(auto& t:scene) for(double& c:t.emission) c=0;
    EXPECT_FALSE(mapper.build(scene)); ASSERT_TRUE(mapper.build(enclosure()));
    auto c=camera(); c.fovDeg=180; Phantom::Graphics::Imagef image;
    EXPECT_FALSE(mapper.renderLinear(c,image));
}
TEST(PhotonMapperTest, GBufferOverridePreservesDirectLightAndReprojectsDepthError)
{
    auto cfg=settings(); cfg.photonMaxDepth=1; PhotonMapper mapper(cfg);
    ASSERT_TRUE(mapper.build(enclosure()));
    PhotonGBuffer gbuffer{1,1,{PhotonReceiver{V(1,0,1),V(0,1,0),4,0.5f}}};
    Phantom::Graphics::Imagef kd,zero(1,1),baseline,added(1,1),result;
    zero.setColor(0,0,Phantom::Graphics::ColorRGBAf(0,0,0,1));
    added.setColor(0,0,Phantom::Graphics::ColorRGBAf(0.1f,0.2f,0.3f,1));
    ASSERT_TRUE(mapper.shadeGBuffer(gbuffer,kd));
    ASSERT_TRUE(mapper.shadeGBuffer(gbuffer,baseline,&zero));
    EXPECT_EQ(kd.getValues(),baseline.getValues()); EXPECT_GT(baseline.getColor(0,0).x,0);
    ASSERT_TRUE(mapper.shadeGBuffer(gbuffer,result,&added));
    for(int c=0;c<3;++c) EXPECT_NEAR(result.getColor(0,0)[c]-baseline.getColor(0,0)[c],0.1*(c+1),1e-6);
    gbuffer.receivers[0].position.y=-0.001; // Float depth can reconstruct inside the surface.
    ASSERT_TRUE(mapper.shadeGBuffer(gbuffer,result,&zero));
    EXPECT_EQ(result.getValues(),baseline.getValues());
    gbuffer.receivers[0].triangle=1000; EXPECT_FALSE(mapper.shadeGBuffer(gbuffer,result,&zero));
}
