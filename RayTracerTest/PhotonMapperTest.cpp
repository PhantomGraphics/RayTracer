#include "pch.h"
#include "../RayTracer/PhotonMapper.h"
#include "../RayTracer/PbvrResampler.h"
#include <algorithm>
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

TEST(PhotonMapperTest, ExternalPhotonsSkipTransportAndPreserveAlias)
{
    auto cfg=settings(); cfg.buildPhotonIndex=false;
    PhotonMapper mapper(cfg); Photon p; p.bounce=1; p.contribution=PhotonContribution::Indirect;
    p.position=V(1,0,1); p.normal=V(0,1,0); p.direction=V(0,-1,0); p.power=V(1,2,3);
    ASSERT_TRUE(mapper.buildWithPhotons(enclosure(),{p}));
    EXPECT_EQ(mapper.getStats().tracedRays,0u); EXPECT_EQ(mapper.getStats().emittedPhotons,0u);
    EXPECT_EQ(mapper.getStats().indirectPhotons,1u);
    ASSERT_TRUE(mapper.buildWithPhotons(enclosure(),mapper.getPhotonMap().getPhotons()));
    EXPECT_EQ(mapper.getPhotonMap().getPhotons().size(),1u);
    ASSERT_TRUE(mapper.buildWithPhotons(enclosure(),{})); EXPECT_TRUE(mapper.getPhotonMap().getPhotons().empty());
    p.bounce=cfg.photonMaxDepth;
    EXPECT_FALSE(mapper.buildWithPhotons(enclosure(),{p})); EXPECT_TRUE(mapper.getPhotonMap().getPhotons().empty());
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

TEST(PhotonMapperTest, PbvrStoresArrivalsBeforeCappingAndCompensatesColoredFlux)
{
    auto scene=enclosure(V(0.5,0.25,0.125));
    quad(scene,V(0,0,0),V(2,0,0),V(0,2,0),V(0.5,0.25,0.125)); // Close front.
    auto cfg=settings(); cfg.transport=PhotonTransport::Pbvr; cfg.photonMaxDepth=2;
    cfg.pbvr.retention=1; cfg.pbvr.maxParticles=137;
    PhotonMapper mapper(cfg); ASSERT_TRUE(mapper.build(scene))<<mapper.getLastError();
    const auto& stats=mapper.getStats(); ASSERT_EQ(stats.bounces.size(),2u);
    EXPECT_EQ(stats.bounces[0].storedPhotons,cfg.photonCount);
    EXPECT_EQ(stats.bounces[0].continuationCandidates,cfg.photonCount);
    EXPECT_EQ(stats.bounces[0].retainedParticles,137u);
    EXPECT_EQ(stats.bounces[1].tracedRays,137u);
    EXPECT_EQ(stats.tracedRays,cfg.photonCount+137);
    const double initial=4*0.36*std::acos(-1.0)/cfg.photonCount;
    for(int c=0;c<3;++c) EXPECT_NEAR(stats.bounces[0].reemittedFlux[c],
        stats.bounces[0].storedFlux[c]*V(0.5,0.25,0.125)[c],1e-10);
    for(const auto& p:mapper.getPhotonMap().getPhotons()) {
        const V expected=p.bounce==0?V(initial):V(initial)*V(0.5,0.25,0.125)*(static_cast<double>(cfg.photonCount)/137);
        for(int c=0;c<3;++c) EXPECT_NEAR(p.power[c],expected[c],1e-12);
    }
}
TEST(PhotonMapperTest, PbvrThinningKeepsFirstArrivalsAndIsReproducible)
{
    auto cfg=settings(); cfg.transport=PhotonTransport::Pbvr; cfg.pbvr.retention=1;
    PhotonMapper full(cfg); ASSERT_TRUE(full.build(enclosure()));
    cfg.pbvr.retention=0.5; cfg.pbvr.maxParticles=300;
    PhotonMapper thin(cfg),repeat(cfg); ASSERT_TRUE(thin.build(enclosure())); ASSERT_TRUE(repeat.build(enclosure()));
    EXPECT_LT(thin.getStats().tracedRays,full.getStats().tracedRays);
    EXPECT_EQ(thin.getStats().directPhotons,full.getStats().directPhotons);
    const auto& a=thin.getPhotonMap().getPhotons(); const auto& b=repeat.getPhotonMap().getPhotons();
    const auto& f=full.getPhotonMap().getPhotons(); ASSERT_EQ(a.size(),b.size());
    std::size_t first=0;
    for(std::size_t i=0;i<a.size();++i) {
        EXPECT_EQ(a[i].position,b[i].position); EXPECT_EQ(a[i].power,b[i].power);
        EXPECT_EQ(a[i].bounce,b[i].bounce);
        if(a[i].bounce==0) { EXPECT_EQ(a[i].position,f[first].position); EXPECT_EQ(a[i].power,f[first].power); ++first; }
    }
    EXPECT_EQ(first,full.getStats().directPhotons);
    for(const auto& step:thin.getStats().bounces) EXPECT_LE(step.retainedParticles,300u);
}
TEST(PhotonMapperTest, PbvrUnthinnedIgnoresSelectionSeedAndPreservesMirrorClassification)
{
    auto scene=enclosure(); for(int i=4;i<6;++i) { scene[i].metallic=1; scene[i].roughness=0; }
    auto cfg=settings(); cfg.transport=PhotonTransport::Pbvr; cfg.pbvr.retention=1;
    PhotonMapper a(cfg); ASSERT_TRUE(a.build(scene));
    cfg.pbvr.randomSeed=999; PhotonMapper b(cfg); ASSERT_TRUE(b.build(scene));
    ASSERT_GT(a.getStats().causticPhotons,0u);
    const auto& pa=a.getPhotonMap().getPhotons(); const auto& pb=b.getPhotonMap().getPhotons();
    ASSERT_EQ(pa.size(),pb.size());
    for(std::size_t i=0;i<pa.size();++i) { EXPECT_EQ(pa[i].position,pb[i].position); EXPECT_EQ(pa[i].power,pb[i].power); }
}
TEST(PhotonMapperTest, PbvrEnsemblePreservesIndirectFlux)
{
    auto cfg=settings(); cfg.photonCount=1000; cfg.photonMaxDepth=4;
    cfg.transport=PhotonTransport::Pbvr; cfg.pbvr.retention=1;
    PhotonMapper full(cfg); ASSERT_TRUE(full.build(enclosure()));
    std::vector<V> sum(4,V(0)); constexpr int ensembles=96;
    const V positions[]={V(1,0,1),V(0,1,1),V(1,1,2)};
    const V normals[]={V(0,1,0),V(1,0,0),V(0,0,-1)};
    V spatialSum[3]={V(0),V(0),V(0)},spatialSquares[3]={V(0),V(0),V(0)};
    cfg.pbvr.retention=0.65; cfg.pbvr.maxParticles=300;
    for(int e=0;e<ensembles;++e) {
        cfg.pbvr.randomSeed=static_cast<std::uint32_t>(e+100);
        PhotonMapper thin(cfg); ASSERT_TRUE(thin.build(enclosure()));
        for(int bounce=0;bounce<4;++bounce) sum[bounce]+=thin.getStats().bounces[bounce].storedFlux;
        for(int q=0;q<3;++q) {
            const V value=thin.getPhotonMap().estimateRadiance(positions[q],normals[q],V(0.7),0.6,PhotonContribution::Indirect);
            spatialSum[q]+=value; spatialSquares[q]+=value*value;
        }
    }
    for(int bounce=0;bounce<4;++bounce) for(int c=0;c<3;++c) {
        const double expected=full.getStats().bounces[bounce].storedFlux[c];
        EXPECT_NEAR(sum[bounce][c]/ensembles,expected,expected*0.08+1e-12)<<bounce<<','<<c;
    }
    for(int q=0;q<3;++q) {
        const V expected=full.getPhotonMap().estimateRadiance(positions[q],normals[q],V(0.7),0.6,PhotonContribution::Indirect);
        for(int c=0;c<3;++c) {
            const double mean=spatialSum[q][c]/ensembles;
            const double variance=std::max(0.0,spatialSquares[q][c]/ensembles-mean*mean);
            const double standardError=std::sqrt(variance/(ensembles-1));
            ASSERT_GT(expected[c],0);
            EXPECT_LT(standardError,expected[c]*0.15);
            EXPECT_NEAR(mean,expected[c],5*standardError+1e-12)<<q<<','<<c;
        }
    }
}
TEST(PhotonMapperTest, PbvrAbsorptionDepthLimitAndInvalidRetention)
{
    auto cfg=settings(); cfg.transport=PhotonTransport::Pbvr;
    PhotonMapper black(cfg); ASSERT_TRUE(black.build(enclosure(V(0))));
    EXPECT_EQ(black.getStats().tracedRays,cfg.photonCount); EXPECT_EQ(black.getStats().indirectPhotons,0u);
    cfg.photonMaxDepth=1; PhotonMapper one(cfg); ASSERT_TRUE(one.build(enclosure()));
    EXPECT_EQ(one.getStats().tracedRays,cfg.photonCount); EXPECT_EQ(one.getStats().bounces[0].retainedParticles,0u);
    for(double bad: {0.0,-0.1,1.1,std::numeric_limits<double>::quiet_NaN()}) {
        cfg.pbvr.retention=bad; PhotonMapper invalid(cfg); EXPECT_FALSE(invalid.build(enclosure()));
        EXPECT_TRUE(invalid.getPhotonMap().getPhotons().empty());
    }
}

TEST(PhotonMapTest, IndexlessScanAndMoveBuildPreserveRadiance)
{
    Photon a; a.power=V(1,2,3); Photon b=a; b.position=V(0.2,0,0); b.power*=2;
    std::vector<Photon> photons{a,b}; PhotonMap indexed,flat;
    ASSERT_TRUE(indexed.build(photons)); ASSERT_TRUE(flat.build(std::move(photons),false));
    EXPECT_TRUE(indexed.hasSpatialIndex()); EXPECT_FALSE(flat.hasSpatialIndex());
    EXPECT_EQ(indexed.getPhotons().size(),flat.getPhotons().size());
    EXPECT_EQ(indexed.estimateRadiance(V(0),V(0,1,0),V(0.7),0.5),flat.estimateRadiance(V(0),V(0,1,0),V(0.7),0.5));
    ASSERT_TRUE(flat.build(flat.getPhotons(),true)); EXPECT_TRUE(flat.hasSpatialIndex());
    flat.clear(); EXPECT_FALSE(flat.hasSpatialIndex());
}
TEST(PhotonMapperTest, GpuStorageOptionsKeepIndirectTransportAndShading)
{
    auto cfg=settings(); cfg.transport=PhotonTransport::Pbvr; cfg.pbvr.retention=0.8;
    PhotonMapper original(cfg); ASSERT_TRUE(original.build(enclosure()));
    cfg.buildPhotonIndex=false; cfg.storeDirectPhotons=false;
    PhotonMapper gpuOnly(cfg); ASSERT_TRUE(gpuOnly.build(enclosure()));
    EXPECT_FALSE(gpuOnly.getPhotonMap().hasSpatialIndex()); EXPECT_EQ(gpuOnly.getStats().directPhotons,0u);
    EXPECT_EQ(gpuOnly.getStats().tracedRays,original.getStats().tracedRays);
    const auto& subset=gpuOnly.getPhotonMap().getPhotons(); std::size_t i=0;
    for(const auto& p:original.getPhotonMap().getPhotons()) if(p.contribution!=PhotonContribution::Direct) {
        ASSERT_LT(i,subset.size()); EXPECT_EQ(p.position,subset[i].position); EXPECT_EQ(p.power,subset[i].power); ++i;
    }
    EXPECT_EQ(i,subset.size());
    PhotonGBuffer gbuffer{1,1,{PhotonReceiver{V(1,0,1),V(0,1,0),4,0.5f}}};
    Phantom::Graphics::Imagef indexed,scan;
    ASSERT_TRUE(original.shadeGBuffer(gbuffer,indexed)); ASSERT_TRUE(gpuOnly.shadeGBuffer(gbuffer,scan));
    EXPECT_EQ(indexed.getValues(),scan.getValues());
}
TEST(PbvrResamplerTest, PowerCompensationPreservesGrayFluxAndStratifiesDraws)
{
    std::vector<PbvrArrival> arrivals;
    for(int i=0;i<100;++i) arrivals.push_back({V(i,0,0),V(1),0});
    PbvrPhotonSettings cfg; cfg.selection=PbvrSelection::Power; cfg.retention=0.5; cfg.uniformMix=0;
    std::vector<PbvrSelectedSample> samples; PbvrResampleStats stats; std::string error;
    ASSERT_TRUE(resamplePbvr(arrivals,cfg,42,samples,stats,error))<<error;
    ASSERT_EQ(samples.size(),50u); V total(0); std::vector<bool> pairs(50,false);
    std::vector<std::uint64_t> keys;
    for(const auto& s:samples) {
        EXPECT_NEAR(s.powerScale,2,1e-12); EXPECT_TRUE(s.resampled);
        EXPECT_FALSE(pairs[s.source/2]); pairs[s.source/2]=true;
        total+=arrivals[s.source].power*s.powerScale; keys.push_back(s.randomKey);
    }
    for(int c=0;c<3;++c) EXPECT_NEAR(total[c],100,1e-10);
    std::sort(keys.begin(),keys.end()); EXPECT_EQ(std::unique(keys.begin(),keys.end()),keys.end());
    arrivals[0].power=V(100); cfg.maxParticles=7;
    ASSERT_TRUE(resamplePbvr(arrivals,cfg,17,samples,stats,error)); total=V(0);
    for(const auto& s:samples) total+=arrivals[s.source].power*s.powerScale;
    for(int c=0;c<3;++c) EXPECT_NEAR(total[c],199,1e-10);
}
TEST(PbvrResamplerTest, SpatialCoverageAndProbabilityCorrectionPreserveColoredFlux)
{
    std::vector<PbvrArrival> arrivals;
    // Different cells / surfaces: each group has a different gray-scaled color.
    for(int i=0;i<20;++i) arrivals.push_back({V(0.1,0.1,0.1),V(1,0.1,0.05)*(i+1.0),0});
    for(int i=0;i<5;++i) arrivals.push_back({V(1.1,0.1,0.1),V(0.1,1,0.05)*(i+1.0),0});
    arrivals.push_back({V(0.1,0.1,0.1),V(0.1,0.05,1),1}); // Same position, different surface.
    V expected(0); for(const auto& a:arrivals) expected+=a.power;
    PbvrPhotonSettings cfg; cfg.selection=PbvrSelection::SpatialPower; cfg.spatialCellSize=1;
    cfg.retention=0.5; cfg.maxParticles=7; cfg.uniformMix=0;
    std::vector<PbvrSelectedSample> samples; PbvrResampleStats stats; std::string error;
    ASSERT_TRUE(resamplePbvr(arrivals,cfg,42,samples,stats,error))<<error;
    EXPECT_EQ(samples.size(),7u); EXPECT_EQ(stats.strata,3u); EXPECT_EQ(stats.sampledStrata,3u);
    V actual(0); for(const auto& s:samples) actual+=arrivals[s.source].power*s.powerScale;
    for(int c=0;c<3;++c) EXPECT_NEAR(actual[c],expected[c],1e-10);
}
TEST(PbvrResamplerTest, BudgetBelowStrataCountIsUnbiasedAndNeverRelocatesPoints)
{
    std::vector<PbvrArrival> arrivals; V expected(0);
    for(int i=0;i<12;++i) { const V power(i+1,13-i,0.5+i%3); arrivals.push_back({V(i+0.1,0,0),power,0}); expected+=power; }
    PbvrPhotonSettings cfg; cfg.selection=PbvrSelection::SpatialPower; cfg.spatialCellSize=1;
    cfg.retention=1; cfg.maxParticles=3; cfg.uniformMix=0.1;
    constexpr int repeats=1024; V sum(0),squares(0);
    std::vector<PbvrSelectedSample> samples; PbvrResampleStats stats; std::string error;
    for(int e=0;e<repeats;++e) {
        ASSERT_TRUE(resamplePbvr(arrivals,cfg,static_cast<std::uint32_t>(e),samples,stats,error));
        EXPECT_EQ(samples.size(),3u); EXPECT_EQ(stats.strata,12u); EXPECT_LE(stats.sampledStrata,3u);
        V total(0); for(const auto& s:samples) { ASSERT_LT(s.source,arrivals.size()); total+=arrivals[s.source].power*s.powerScale; }
        sum+=total; squares+=total*total;
    }
    for(int c=0;c<3;++c) {
        const double mean=sum[c]/repeats,se=std::sqrt(std::max(0.0,squares[c]/repeats-mean*mean)/(repeats-1));
        EXPECT_LT(se,expected[c]*0.03); EXPECT_NEAR(mean,expected[c],5*se+1e-10);
    }
}
TEST(PbvrResamplerTest, IdentityEmptyReproducibilityAndInvalidInputs)
{
    std::vector<PbvrArrival> arrivals{{V(0),V(1),0},{V(1),V(2),0}};
    PbvrPhotonSettings cfg; cfg.selection=PbvrSelection::Power; cfg.retention=1;
    std::vector<PbvrSelectedSample> a,b; PbvrResampleStats stats; std::string error;
    ASSERT_TRUE(resamplePbvr(arrivals,cfg,42,a,stats,error)); EXPECT_EQ(a.size(),2u);
    for(std::size_t i=0;i<2;++i) { EXPECT_EQ(a[i].source,i); EXPECT_EQ(a[i].powerScale,1); EXPECT_FALSE(a[i].resampled); }
    cfg.retention=0.5;
    ASSERT_TRUE(resamplePbvr(arrivals,cfg,42,a,stats,error)); ASSERT_TRUE(resamplePbvr(arrivals,cfg,42,b,stats,error));
    EXPECT_EQ(a[0].source,b[0].source); EXPECT_EQ(a[0].powerScale,b[0].powerScale); EXPECT_EQ(a[0].randomKey,b[0].randomKey);
    ASSERT_TRUE(resamplePbvr({},cfg,42,a,stats,error)); EXPECT_TRUE(a.empty());
    cfg.uniformMix=1.1; EXPECT_FALSE(resamplePbvr(arrivals,cfg,42,a,stats,error)); EXPECT_TRUE(a.empty());
    cfg.uniformMix=0.05; cfg.selection=PbvrSelection::SpatialPower; cfg.spatialCellSize=0;
    EXPECT_FALSE(resamplePbvr(arrivals,cfg,42,a,stats,error));
    cfg.spatialCellSize=1e-30; EXPECT_FALSE(resamplePbvr(arrivals,cfg,42,a,stats,error));
    cfg.spatialCellSize=1; arrivals[0].power.x=-1;
    EXPECT_FALSE(resamplePbvr(arrivals,cfg,42,a,stats,error)); EXPECT_EQ(stats.strata,0u);
}
TEST(PbvrResamplerTest, ColoredImportanceWeightsAreUnbiasedWithinCoveredStrata)
{
    std::vector<PbvrArrival> arrivals; V expected(0);
    for(int i=0;i<24;++i) {
        const V power(0.1+i%5,0.2+(i*3)%7,0.05+(i*5)%3);
        arrivals.push_back({V(i%3+0.1,0,0),power,0}); expected+=power;
    }
    PbvrPhotonSettings cfg; cfg.retention=0.5; cfg.maxParticles=5; cfg.uniformMix=0.05; cfg.spatialCellSize=1;
    std::vector<PbvrSelectedSample> samples; PbvrResampleStats stats; std::string error;
    for(auto mode:{PbvrSelection::Power,PbvrSelection::SpatialPower}) {
        cfg.selection=mode; V sum(0),squares(0); constexpr int repeats=1024;
        for(int e=0;e<repeats;++e) {
            ASSERT_TRUE(resamplePbvr(arrivals,cfg,static_cast<std::uint32_t>(e),samples,stats,error));
            if(mode==PbvrSelection::SpatialPower) EXPECT_EQ(stats.sampledStrata,3u);
            V value(0); for(const auto& sample:samples) value+=arrivals[sample.source].power*sample.powerScale;
            sum+=value; squares+=value*value;
        }
        for(int c=0;c<3;++c) {
            const double mean=sum[c]/repeats,se=std::sqrt(std::max(0.0,squares[c]/repeats-mean*mean)/(repeats-1));
            EXPECT_LT(se,expected[c]*0.04); EXPECT_NEAR(mean,expected[c],5*se+1e-10);
        }
    }
}
TEST(PhotonMapperTest, ImportanceTransportIsReproducibleAndPreservesMirrorPaths)
{
    auto scene=enclosure(); for(int i=4;i<6;++i) { scene[i].metallic=1; scene[i].roughness=0; }
    auto cfg=settings(); cfg.transport=PhotonTransport::Pbvr; cfg.pbvr.maxParticles=500;
    for(auto mode:{PbvrSelection::Power,PbvrSelection::SpatialPower}) {
        cfg.pbvr.selection=mode; cfg.pbvr.spatialCellSize=0.35;
        PhotonMapper a(cfg),b(cfg); ASSERT_TRUE(a.build(scene))<<a.getLastError(); ASSERT_TRUE(b.build(scene));
        EXPECT_GT(a.getStats().causticPhotons,0u);
        for(const auto& step:a.getStats().bounces) EXPECT_LE(step.retainedParticles,500u);
        const auto& pa=a.getPhotonMap().getPhotons(); const auto& pb=b.getPhotonMap().getPhotons(); ASSERT_EQ(pa.size(),pb.size());
        for(std::size_t i=0;i<pa.size();++i) { EXPECT_EQ(pa[i].position,pb[i].position); EXPECT_EQ(pa[i].power,pb[i].power); EXPECT_EQ(pa[i].direction,pb[i].direction); }
    }
}
