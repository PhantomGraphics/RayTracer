#include "PhotonMapper.h"
#include "PbvrResampler.h"
#include "../../CGLib/Space/Space/KDTree.h"
#include "../../CGLib/Space/Space/BVH.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>

namespace Phantom::RayTracer {
namespace {
using V = Math::Vector3dd;
constexpr double pi = 3.14159265358979323846;
bool finite(V v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool nonnegative(V v) { return finite(v) && v.x >= 0 && v.y >= 0 && v.z >= 0; }
bool unitColor(V v) { return nonnegative(v) && v.x <= 1 && v.y <= 1 && v.z <= 1; }
double maximum(V v) { return std::max({v.x, v.y, v.z}); }
V vector(const double* p) { return V(p[0],p[1],p[2]); }
struct Random {
    std::mt19937 engine;
    explicit Random(std::uint32_t seed) : engine(seed) {}
    double next() { return (static_cast<double>(engine())+0.5)/4294967296.0; }
};
// Small per-particle stream: surviving paths are independent of the selection
// and ordering of other particles. This permits paired thinning comparisons.
struct ParticleRandom {
    std::uint64_t state;
    double next() {
        std::uint64_t z=(state+=0x9e3779b97f4a7c15ull);
        z=(z^(z>>30))*0xbf58476d1ce4e5b9ull;
        z=(z^(z>>27))*0x94d049bb133111ebull;
        z^=z>>31;
        return (static_cast<double>(z>>12)+0.5)/4503599627370496.0;
    }
};
template<class R> V cosine(V normal, R& rng)
{
    const double r=std::sqrt(rng.next()), phi=2*pi*rng.next();
    const V tangent=glm::normalize(glm::cross(std::abs(normal.y)<0.9 ? V(0,1,0) : V(1,0,0),normal));
    return tangent*(r*std::cos(phi))+glm::cross(normal,tangent)*(r*std::sin(phi))
        +normal*std::sqrt(std::max(0.0,1-r*r));
}
bool accepts(PhotonContribution wanted, PhotonContribution actual)
{ return wanted==PhotonContribution::All || wanted==actual
    || (wanted==PhotonContribution::Indirect && actual==PhotonContribution::Caustic); }
}
struct PhotonMap::Impl { std::vector<Photon> photons; Space::KDTree tree; bool indexed=false; };
PhotonMap::PhotonMap() : impl_(std::make_unique<Impl>()) {}
PhotonMap::~PhotonMap() = default;
void PhotonMap::clear() { impl_->photons.clear(); impl_->tree.clear(); impl_->indexed=false; }
const std::vector<Photon>& PhotonMap::getPhotons() const { return impl_->photons; }
bool PhotonMap::hasSpatialIndex() const { return impl_->indexed; }
bool PhotonMap::build(const std::vector<Photon>& photons,bool buildIndex)
{ return build(std::vector<Photon>(photons),buildIndex); }
bool PhotonMap::build(std::vector<Photon>&& photons,bool buildIndex)
{
    if(photons.size()>static_cast<std::size_t>(std::numeric_limits<int>::max())) { clear(); return false; }
    std::vector<Photon> validated=std::move(photons);
    clear();
    Math::Vector3dfVector positions; if(buildIndex) positions.reserve(validated.size());
    for(const auto& p:validated) {
        const Math::Vector3df position(p.position);
        if(!finite(p.position) || !finite(V(position)) || !nonnegative(p.power)
            || !finite(p.normal) || !finite(p.direction)
            || std::abs(glm::length(p.normal)-1)>1e-6 || std::abs(glm::length(p.direction)-1)>1e-6
            || (p.contribution!=PhotonContribution::Direct && p.contribution!=PhotonContribution::Indirect
                && p.contribution!=PhotonContribution::Caustic) || p.bounce<0 || p.bounce>=64) return false;
        if(buildIndex) positions.push_back(position);
    }
    impl_->photons=std::move(validated);
    if(buildIndex) impl_->tree.build(positions);
    impl_->indexed=buildIndex; return true;
}
V PhotonMap::estimateRadiance(const V& position,const V& normal,const V& reflectance,
                             double radius,PhotonContribution contribution) const
{
    if(!finite(position) || !finite(normal) || !unitColor(reflectance) || !std::isfinite(radius)
        || radius<=0 || radius>std::numeric_limits<float>::max()
        || std::abs(glm::length(normal)-1)>1e-6) return V(0);
    const Math::Vector3df query(position); if(!finite(V(query))) return V(0);
    V flux(0);
    // Cover rounding of both the float query and stored positions; final filtering
    // uses the original double coordinates and radius.
    const double padding=8*std::numeric_limits<float>::epsilon()*std::max(1.0,maximum(glm::abs(position))+radius);
    const float queryRadius=std::nextafter(static_cast<float>(std::min(radius+padding,
        static_cast<double>(std::numeric_limits<float>::max()))),std::numeric_limits<float>::infinity());
    const auto accumulate=[&](const Photon& p) {
        const V delta=p.position-position;
        if(!accepts(contribution,p.contribution) || glm::dot(delta,delta)>radius*radius
            || glm::dot(p.normal,normal)<0.9 || glm::dot(p.direction,normal)>=0
            || std::abs(glm::dot(delta,normal))>radius*0.02) return;
        flux+=p.power;
    };
    if(impl_->indexed) {
        for(int index:impl_->tree.findWithinRadius(query,queryRadius)) accumulate(impl_->photons[index]);
    } else {
        for(const auto& p:impl_->photons) accumulate(p);
    }
    // Photons carry incident flux: do not multiply by another incident cosine.
    return reflectance*flux/(pi*pi*radius*radius);
}

struct PhotonMapper::Impl {
    struct Triangle { V a,b,c,normal,reflectance,emission; double area=0; bool mirror=false; };
    struct Light { int index; double probability,cumulative; };
    struct Hit { int index=-1; double distance; V position{0}; };
    PhotonMappingSettings settings;
    PhotonMappingStats stats;
    PhotonMap map;
    std::string error;
    std::vector<Triangle> triangles;
    std::vector<Light> lights;
    std::vector<std::unique_ptr<Space::BVHObject>> objects;
    std::unique_ptr<Space::BVH> bvh;
    bool ready=false;
    explicit Impl(const PhotonMappingSettings& s) : settings(s) {}
    bool fail(const char* message) { error=message; return false; }
    Hit hit(V origin,V direction,double limit=1e30) const
    {
        Hit result; result.distance=limit;
        for(const auto* object:bvh->queryRay(Math::Vector3df(origin),Math::Vector3df(direction),
                    static_cast<float>(settings.rayEpsilon),static_cast<float>(std::min(limit,1e30)))) {
            const auto& t=triangles[object->id];
            const V e1=t.b-t.a,e2=t.c-t.a,p=glm::cross(direction,e2);
            const double determinant=glm::dot(e1,p);
            if(std::abs(determinant)<1e-14) continue;
            const double inverse=1/determinant; const V offset=origin-t.a;
            const double u=glm::dot(offset,p)*inverse;
            if(u<0 || u>1) continue;
            const V q=glm::cross(offset,e1); const double v=glm::dot(direction,q)*inverse;
            if(v<0 || u+v>1) continue;
            const double distance=glm::dot(e2,q)*inverse;
            if(distance<=settings.rayEpsilon || distance>=result.distance) continue;
            result={object->id,distance,origin+direction*distance};
        }
        return result;
    }
    template<class R> const Light& selectLight(R& rng) const
    {
        const double sample=rng.next();
        const auto it=std::lower_bound(lights.begin(),lights.end(),sample,
            [](const Light& l,double x) { return l.cumulative<x; });
        return it==lights.end()?lights.back():*it;
    }
    template<class R> V point(const Triangle& t,R& rng) const
    { const double a=std::sqrt(rng.next()),b=rng.next(); return t.a*(1-a)+t.b*(a*(1-b))+t.c*(a*b); }
    V direct(const Hit& h,Random& rng) const
    {
        const auto& receiver=triangles[h.index]; V sum(0);
        for(int i=0;i<settings.directLightSamples;++i) {
            const auto& light=selectLight(rng); const auto& source=triangles[light.index];
            const V origin=h.position+receiver.normal*(settings.rayEpsilon*4);
            const V delta=point(source,rng)-origin; const double distance=glm::length(delta);
            if(distance<=settings.rayEpsilon*8) continue;
            const V direction=delta/distance;
            const double geometry=std::max(0.0,glm::dot(receiver.normal,direction))
                *std::max(0.0,glm::dot(source.normal,-direction))/(distance*distance);
            if(geometry<=0 || hit(origin,direction,distance-settings.rayEpsilon*2).index>=0) continue;
            sum+=receiver.reflectance*source.emission*(source.area*geometry/(pi*light.probability));
        }
        return sum/static_cast<double>(settings.directLightSamples);
    }
    V cameraRadiance(V origin,V direction,Random& rng) const
    {
        V weight(1),emitted(0);
        for(int depth=0;depth<settings.render.maxDepth;++depth) {
            const Hit h=hit(origin,direction); if(h.index<0) return emitted;
            const auto& t=triangles[h.index]; if(glm::dot(t.normal,-direction)<=0) return emitted;
            emitted+=weight*t.emission;
            if(!t.mirror) return emitted+weight*(direct(h,rng)+map.estimateRadiance(h.position,t.normal,
                t.reflectance,settings.gatherRadius,PhotonContribution::Indirect));
            weight*=t.reflectance; origin=h.position+t.normal*(settings.rayEpsilon*4);
            direction=glm::reflect(direction,t.normal);
        }
        return emitted;
    }
    void store(std::vector<Photon>& photons,const Hit& h,V direction,V power,
               bool diffuseSeen,bool specularSeen,int bounce) {
        const auto contribution=diffuseSeen?PhotonContribution::Indirect:
            (specularSeen?PhotonContribution::Caustic:PhotonContribution::Direct);
        if(contribution==PhotonContribution::Direct && !settings.storeDirectPhotons) return;
        photons.push_back({h.position,direction,triangles[h.index].normal,power,contribution,bounce});
        if(contribution==PhotonContribution::Direct) ++stats.directPhotons;
        else if(contribution==PhotonContribution::Caustic) ++stats.causticPhotons;
        else ++stats.indirectPhotons;
        auto& step=stats.bounces[bounce]; ++step.storedPhotons; step.storedFlux+=power;
    }
    bool tracePbvr(std::vector<Photon>& photons) {
        struct Particle {
            V origin,direction,power;
            ParticleRandom random;
            bool diffuseSeen=false,specularSeen=false;
            int surface=-1; // Set on arrival; reused when re-emitting.
        };
        std::vector<Particle> active; active.reserve(settings.photonCount);
        for(std::size_t i=0;i<settings.photonCount;++i) {
            ParticleRandom rng{(static_cast<std::uint64_t>(settings.render.randomSeed)<<32)^i};
            const auto& light=selectLight(rng); const auto& source=triangles[light.index];
            const V origin=point(source,rng)+source.normal*(settings.rayEpsilon*4);
            const V direction=cosine(source.normal,rng);
            const V power=source.emission*(pi*source.area/(static_cast<double>(settings.photonCount)*light.probability));
            if(!nonnegative(power)) return fail("Nonfinite emitted PBVR power");
            active.push_back({origin,direction,power,rng});
        }
        Random selection(settings.pbvr.randomSeed);
        std::vector<Particle> arrivals;
        for(int bounce=0;bounce<settings.photonMaxDepth && !active.empty();++bounce) {
            auto& step=stats.bounces[bounce]; step.tracedRays=active.size(); stats.tracedRays+=active.size();
            arrivals.clear(); arrivals.reserve(active.size());
            for(auto p:active) {
                const auto h=hit(p.origin,p.direction); if(h.index<0) continue;
                const auto& t=triangles[h.index]; if(glm::dot(t.normal,-p.direction)<=0) continue;
                ++step.arrivals;
                // Every diffuse arrival contributes before any selection for the next order.
                if(!t.mirror) { store(photons,h,p.direction,p.power,p.diffuseSeen,p.specularSeen,bounce); p.diffuseSeen=true; }
                else p.specularSeen=true;
                if(bounce+1==settings.photonMaxDepth) continue;
                p.power*=t.reflectance; // Material absorption is separate from thinning.
                if(maximum(p.power)<=0) continue;
                ++step.continuationCandidates;
                if(settings.pbvr.selection==PbvrSelection::Uniform) {
                    if(settings.pbvr.retention<1 && selection.next()>=settings.pbvr.retention) continue;
                    p.power/=settings.pbvr.retention;
                }
                if(!nonnegative(p.power)) return fail("PBVR selection compensation overflow");
                p.origin=h.position; p.surface=h.index; arrivals.push_back(p);
            }
            const std::size_t cap=settings.pbvr.maxParticles;
            if(settings.pbvr.selection!=PbvrSelection::Uniform && !arrivals.empty()) {
                std::vector<PbvrArrival> candidates; candidates.reserve(arrivals.size());
                for(const auto& p:arrivals) candidates.push_back({p.origin,p.power,p.surface});
                auto options=settings.pbvr;
                if(options.spatialCellSize==0) options.spatialCellSize=settings.gatherRadius;
                std::vector<PbvrSelectedSample> samples; PbvrResampleStats selectionStats;
                const auto seed=settings.pbvr.randomSeed^static_cast<std::uint32_t>(bounce)*0x9e3779b9u;
                if(!resamplePbvr(candidates,options,seed,samples,selectionStats,error)) return false;
                std::vector<Particle> selected; selected.reserve(samples.size());
                for(const auto& sample:samples) {
                    auto p=arrivals[sample.source]; p.power*=sample.powerScale;
                    if(!nonnegative(p.power)) return fail("PBVR importance compensation overflow");
                    if(sample.resampled) p.random.state=sample.randomKey; // Independent directions for duplicate draws.
                    selected.push_back(p);
                }
                arrivals.swap(selected); step.spatialStrata=selectionStats.strata;
                step.sampledStrata=selectionStats.sampledStrata;
            }
            const double capWeight=settings.pbvr.selection==PbvrSelection::Uniform && cap && arrivals.size()>cap?
                static_cast<double>(arrivals.size())/cap:1;
            if(capWeight>1) {
                // Uniform fixed-size sample, not the first K arrivals (which would bias geometry).
                for(std::size_t i=0;i<cap;++i) {
                    std::uniform_int_distribution<std::size_t> choose(i,arrivals.size()-1);
                    std::swap(arrivals[i],arrivals[choose(selection.engine)]);
                }
                arrivals.resize(cap);
            }
            for(auto& p:arrivals) {
                p.power*=capWeight;
                if(!nonnegative(p.power)) return fail("PBVR particle cap compensation overflow");
                const auto& t=triangles[p.surface];
                p.origin+=t.normal*(settings.rayEpsilon*4);
                p.direction=t.mirror?glm::reflect(p.direction,t.normal):cosine(t.normal,p.random);
                step.reemittedFlux+=p.power;
            }
            step.retainedParticles=arrivals.size(); active.swap(arrivals);
        }
        return true;
    }
};
PhotonMapper::PhotonMapper(const PhotonMappingSettings& settings) : impl_(std::make_unique<Impl>(settings)) {}
PhotonMapper::~PhotonMapper() = default;
const PhotonMap& PhotonMapper::getPhotonMap() const { return impl_->map; }
const PhotonMappingStats& PhotonMapper::getStats() const { return impl_->stats; }
const std::string& PhotonMapper::getLastError() const { return impl_->error; }
bool PhotonMapper::build(const std::vector<RtTriangle>& input)
{
    return buildImpl(input,nullptr);
}
bool PhotonMapper::buildWithPhotons(const std::vector<RtTriangle>& input,const std::vector<Photon>& photons)
{
    // Copy first so an alias of our current map remains valid after reset.
    const auto copy=photons;
    return buildImpl(input,&copy);
}
bool PhotonMapper::buildImpl(const std::vector<RtTriangle>& input,const std::vector<Photon>* external)
{
    auto& s=*impl_; const auto start=std::chrono::steady_clock::now();
    s.ready=false; s.map.clear(); s.stats={}; s.error.clear(); s.bvh.reset();
    s.objects.clear(); s.triangles.clear(); s.lights.clear();
    const auto& cfg=s.settings;
    if(cfg.photonCount==0 || cfg.photonMaxDepth<1 || cfg.photonMaxDepth>64
        || cfg.photonCount>static_cast<std::size_t>(std::numeric_limits<int>::max()/cfg.photonMaxDepth)
        || !std::isfinite(cfg.gatherRadius) || cfg.gatherRadius<=0 || cfg.gatherRadius>1e30
        || !std::isfinite(cfg.rayEpsilon) || cfg.rayEpsilon<=0 || cfg.rayEpsilon>1e10
        || cfg.directLightSamples<1 || cfg.render.maxDepth<1 || cfg.render.maxDepth>64)
        return s.fail("Invalid photon mapping settings");
    if(cfg.transport!=PhotonTransport::Path && cfg.transport!=PhotonTransport::Pbvr)
        return s.fail("Invalid photon transport mode");
    if(cfg.transport==PhotonTransport::Pbvr && (!std::isfinite(cfg.pbvr.retention)
        || cfg.pbvr.retention<=0 || cfg.pbvr.retention>1)) return s.fail("PBVR retention must be in (0, 1]");
    if(cfg.transport==PhotonTransport::Pbvr && ((cfg.pbvr.selection!=PbvrSelection::Uniform
        && cfg.pbvr.selection!=PbvrSelection::Power && cfg.pbvr.selection!=PbvrSelection::SpatialPower)
        || !std::isfinite(cfg.pbvr.uniformMix) || cfg.pbvr.uniformMix<0 || cfg.pbvr.uniformMix>1
        || !std::isfinite(cfg.pbvr.spatialCellSize) || cfg.pbvr.spatialCellSize<0)) return s.fail("Invalid PBVR selection settings");
    if(input.empty() || input.size()>static_cast<std::size_t>(std::numeric_limits<int>::max())) return s.fail("Invalid triangle count");
    double total=0;
    for(const auto& t:input) {
        const V a=vector(t.v0),b=vector(t.v1),c=vector(t.v2),reflectance=vector(t.albedo),emission=vector(t.emission);
        if(!finite(a) || !finite(b) || !finite(c) || maximum(glm::abs(a))>1e20 || maximum(glm::abs(b))>1e20 || maximum(glm::abs(c))>1e20
            || !unitColor(reflectance) || !nonnegative(emission) || !std::isfinite(t.roughness)
            || t.roughness<0 || t.roughness>1 || (t.metallic!=0 && !(t.metallic==1 && t.roughness==0))
            || t.baseColorTextureIndex!=-1 || t.metallicRoughnessTextureIndex!=-1
            || t.normalTextureIndex!=-1 || t.emissiveTextureIndex!=-1)
            return s.fail("Invalid triangle or unsupported material (diffuse / ideal mirror only; no textures)");
        const V cross=glm::cross(b-a,c-a); const double area=glm::length(cross)*0.5;
        if(!std::isfinite(area) || area<=1e-14) return s.fail("Degenerate triangle");
        const int index=static_cast<int>(s.triangles.size());
        s.triangles.push_back({a,b,c,cross/(2*area),reflectance,emission,area,t.metallic==1});
        Math::Box3df box(Math::Vector3df(glm::min(a,glm::min(b,c))),Math::Vector3df(glm::max(a,glm::max(b,c))));
        const Math::Vector3df padding(static_cast<float>(cfg.rayEpsilon*4));
        box=Math::Box3df(box.getMin()-padding,box.getMax()+padding);
        s.objects.push_back(std::make_unique<Space::BVHObject>(index,box));
        const double power=area*glm::dot(emission,V(0.2126,0.7152,0.0722));
        if(power>0) { total+=power; s.lights.push_back({index,power,total}); }
    }
    if(s.lights.empty() || !std::isfinite(total)) return s.fail("Scene needs finite, nonzero emissive triangle power");
    for(auto& light:s.lights) { light.probability/=total; light.cumulative/=total; }
    s.lights.back().cumulative=1;
    std::vector<Space::BVHObject*> objects; for(auto& o:s.objects) objects.push_back(o.get());
    s.bvh=std::make_unique<Space::BVH>(objects);
    std::vector<Photon> photons; photons.reserve(cfg.photonCount);
    s.stats.bounces.resize(cfg.photonMaxDepth);
    const auto transportStart=std::chrono::steady_clock::now();
    if(external) {
        photons=*external;
        for(const auto& p:photons) {
            if(p.bounce<0 || p.bounce>=cfg.photonMaxDepth) return s.fail("Invalid external photon bounce");
            auto& step=s.stats.bounces[p.bounce]; ++step.arrivals; ++step.storedPhotons; step.storedFlux+=p.power;
            if(p.contribution==PhotonContribution::Direct) ++s.stats.directPhotons;
            else if(p.contribution==PhotonContribution::Caustic) ++s.stats.causticPhotons;
            else ++s.stats.indirectPhotons;
        }
    } else if(cfg.transport==PhotonTransport::Pbvr) {
        if(!s.tracePbvr(photons)) return false;
    } else {
    Random rng(cfg.render.randomSeed);
    for(std::size_t i=0;i<cfg.photonCount;++i) {
        const auto& light=s.selectLight(rng); const auto& source=s.triangles[light.index];
        V origin=s.point(source,rng)+source.normal*(cfg.rayEpsilon*4),direction=cosine(source.normal,rng);
        V power=source.emission*(pi*source.area/(static_cast<double>(cfg.photonCount)*light.probability));
        bool diffuseSeen=false,specularSeen=false;
        for(int bounce=0;bounce<cfg.photonMaxDepth;++bounce) {
            auto& step=s.stats.bounces[bounce]; ++step.tracedRays; ++s.stats.tracedRays;
            const auto h=s.hit(origin,direction); if(h.index<0) break;
            const auto& t=s.triangles[h.index]; if(glm::dot(t.normal,-direction)<=0) break;
            ++step.arrivals;
            if(!t.mirror) {
                s.store(photons,h,direction,power,diffuseSeen,specularSeen,bounce);
                diffuseSeen=true;
            } else specularSeen=true;
            const double survival=maximum(t.reflectance);
            if(bounce+1<cfg.photonMaxDepth && survival>0) ++step.continuationCandidates;
            if(survival<=0 || rng.next()>=survival) break;
            power*=t.reflectance/survival; // Russian roulette compensation.
            origin=h.position+t.normal*(cfg.rayEpsilon*4);
            direction=t.mirror?glm::reflect(direction,t.normal):cosine(t.normal,rng);
            if(bounce+1<cfg.photonMaxDepth) { ++step.retainedParticles; step.reemittedFlux+=power; }
        }
    }
    }
    s.stats.transportSeconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-transportStart).count();
    const auto mapStart=std::chrono::steady_clock::now();
    if(!s.map.build(std::move(photons),cfg.buildPhotonIndex)) return s.fail("Invalid photon map");
    s.stats.mapBuildSeconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-mapStart).count();
    s.stats.emittedPhotons=external?0:cfg.photonCount;
    s.stats.buildSeconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    s.ready=true; return true;
}
bool PhotonMapper::renderLinear(const RtCameraSpec& camera,Graphics::Imagef& output) const
{
    const auto& s=*impl_; const auto& cfg=s.settings.render;
    const V eye=vector(camera.lookFrom),target=vector(camera.lookAt),up=vector(camera.up);
    if(!s.ready || cfg.width<1 || cfg.height<1 || cfg.width>16384 || cfg.height>16384 || cfg.samplesPerPixel<1
        || !finite(eye) || !finite(target) || !finite(up) || !std::isfinite(camera.fovDeg)
        || camera.fovDeg<=0 || camera.fovDeg>=179 || !finite(target-eye)
        || maximum(glm::abs(eye))>1e20 || maximum(glm::abs(target))>1e20
        || maximum(glm::abs(up))>1e20 || glm::length(target-eye)<1e-12) return false;
    const V forward=glm::normalize(target-eye),cross=glm::cross(forward,up);
    if(glm::length(cross)<1e-12) return false;
    const V right=glm::normalize(cross),vertical=glm::cross(right,forward);
    const double scale=std::tan(camera.fovDeg*pi/360),aspect=static_cast<double>(cfg.width)/cfg.height;
    Graphics::Imagef image(cfg.width,cfg.height);
    for(int y=0;y<cfg.height;++y) for(int x=0;x<cfg.width;++x) {
        Random rng(cfg.randomSeed^0xa511e9b3u^static_cast<std::uint32_t>(y*cfg.width+x)); V sum(0);
        for(int sample=0;sample<cfg.samplesPerPixel;++sample) {
            const double u=(2*(x+rng.next())/cfg.width-1)*aspect*scale;
            const double v=(1-2*(y+rng.next())/cfg.height)*scale;
            sum+=s.cameraRadiance(eye,glm::normalize(forward+right*u+vertical*v),rng);
        }
        const V color=sum/static_cast<double>(cfg.samplesPerPixel);
        if(!nonnegative(color) || maximum(color)>std::numeric_limits<float>::max()) return false;
        image.setColor(x,y,Graphics::ColorRGBAf(static_cast<float>(color.x),static_cast<float>(color.y),static_cast<float>(color.z),1));
    }
    output=std::move(image); return true;
}
bool PhotonMapper::render(const RtCameraSpec& camera,Graphics::Imageuc& output) const
{
    Graphics::Imagef linear; if(!renderLinear(camera,linear)) return false;
    Graphics::Imageuc image(linear.getWidth(),linear.getHeight());
    for(int y=0;y<linear.getHeight();++y) for(int x=0;x<linear.getWidth();++x) {
        const auto c=linear.getColor(x,y); Graphics::ColorRGBAuc color(0,0,0,255);
        for(int channel=0;channel<3;++channel) color[channel]=static_cast<unsigned char>(255.999*std::sqrt(std::clamp(static_cast<double>(c[channel]),0.0,1.0)));
        image.setColor(x,y,color);
    }
    output=std::move(image); return true;
}
bool PhotonMapper::shadeGBuffer(const PhotonGBuffer& gbuffer,Graphics::Imagef& output,
                               const Graphics::Imagef* indirect) const
{
    const auto& s=*impl_;
    if(!s.ready || gbuffer.width<1 || gbuffer.height<1 || gbuffer.width>16384 || gbuffer.height>16384
        || gbuffer.receivers.size()!=static_cast<std::size_t>(gbuffer.width)*gbuffer.height
        || (indirect && (indirect->getWidth()!=gbuffer.width || indirect->getHeight()!=gbuffer.height))) return false;
    Graphics::Imagef image(gbuffer.width,gbuffer.height);
    for(int y=0;y<gbuffer.height;++y) for(int x=0;x<gbuffer.width;++x) {
        const auto& p=gbuffer.receivers[static_cast<std::size_t>(y)*gbuffer.width+x]; V color(0);
        if(p.triangle>=0) {
            if(p.triangle>=static_cast<int>(s.triangles.size()) || !finite(p.position) || !finite(p.normal)
                || std::abs(glm::length(p.normal)-1)>1e-6 || s.triangles[p.triangle].mirror) return false;
            const auto& t=s.triangles[p.triangle];
            if(glm::dot(t.normal,p.normal)<0.999) return false;
            Random rng(s.settings.render.randomSeed^0xa511e9b3u^static_cast<std::uint32_t>(y*gbuffer.width+x));
            V scattered;
            if(indirect) { const auto c=indirect->getColor(x,y); scattered=V(c.x,c.y,c.z); }
            else scattered=s.map.estimateRadiance(p.position,p.normal,t.reflectance,s.settings.gatherRadius,PhotonContribution::Indirect);
            if(!nonnegative(scattered)) return false;
            // Device depth reconstruction has float error. Start shadow rays on
            // the actual triangle plane so that a receiver cannot shadow itself.
            const V surface=p.position-t.normal*glm::dot(p.position-t.a,t.normal);
            color=t.emission+s.direct({p.triangle,0,surface},rng)+scattered;
        }
        if(!nonnegative(color) || maximum(color)>std::numeric_limits<float>::max()) return false;
        image.setColor(x,y,Graphics::ColorRGBAf(static_cast<float>(color.x),static_cast<float>(color.y),static_cast<float>(color.z),1));
    }
    output=std::move(image); return true;
}
} // namespace Phantom::RayTracer
