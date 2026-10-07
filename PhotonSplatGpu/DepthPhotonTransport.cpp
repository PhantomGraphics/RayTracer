#include "DepthPhotonTransport.h"
#include "../RayTracer/PbvrResampler.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>
#include <utility>

namespace Phantom::RayTracer {
namespace {
using V=Math::Vector3dd;
constexpr double pi=3.14159265358979323846;
V vec(const double* p) { return V(p[0],p[1],p[2]); }
bool finite(V p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
bool positive(V p) { return finite(p)&&std::min({p.x,p.y,p.z})>=0&&std::max({p.x,p.y,p.z})>0; }
struct Source { V position,normal,power; };
struct Face { V forward,up,right; };
std::vector<Face> faces(V n) {
    const V axis=std::abs(n.y)<0.9?V(0,1,0):V(1,0,0);
    const V t=glm::normalize(glm::cross(n,axis)),b=glm::cross(n,t);
    std::vector<Face> result;
    for(const auto& f:std::vector<std::pair<V,V>>{{n,b},{t,n},{-t,n},{b,n},{-b,n}}) {
        const V right=glm::normalize(glm::cross(f.first,f.second));
        result.push_back({f.first,glm::cross(right,f.first),right});
    }
    return result;
}
struct Pixel { V direction; double weight; };
std::vector<Pixel> quadrature(const std::vector<Face>& fs,V n,int edge) {
    std::vector<Pixel> pixels; pixels.reserve(static_cast<std::size_t>(5)*edge*edge);
    double total=0;
    for(const auto& f:fs) for(int y=0;y<edge;++y) for(int x=0;x<edge;++x) {
        const double u=2*(x+0.5)/edge-1,v=2*(y+0.5)/edge-1,r2=1+u*u+v*v;
        const V direction=(f.forward+f.right*u-f.up*v)/std::sqrt(r2);
        const double w=std::max(0.0,glm::dot(n,direction))*4/(edge*edge*std::pow(r2,1.5));
        pixels.push_back({direction,w}); total+=w;
    }
    // Normalize the discrete hemisphere, including rays which escape or clip.
    for(auto& p:pixels) p.weight/=total;
    return pixels;
}
}
bool DepthPhotonTransport::build(PhotonSplatGpu& gpu,const std::vector<RtTriangle>& triangles,
    const DepthPhotonSettings& cfg,PhotonMap& output)
{
    output.clear(); stats_={}; error_.clear();
    const auto start=std::chrono::steady_clock::now();
    const auto fail=[&](const std::string& message) { error_=message; stats_={}; output.clear(); return false; };
    if(triangles.empty() || triangles.size()>1000000 || cfg.lightSamples<1 || cfg.lightSamples>100000
        || cfg.maxDepth<1 || cfg.maxDepth>64 || cfg.firstResolution<2 || cfg.firstResolution>1024
        || cfg.minResolution<2 || cfg.minResolution>cfg.firstResolution || cfg.resolutionDropEvery<0
        || !std::isfinite(cfg.nearPlane) || cfg.nearPlane<=0 || !std::isfinite(cfg.farPlane)
        || cfg.farPlane<=cfg.nearPlane || cfg.farPlane>1e10 || cfg.farPlane/cfg.nearPlane>1e7
        || !std::isfinite(cfg.sourceOffset) || cfg.sourceOffset<=0 || cfg.maxStoredPhotons==0
        || !std::isfinite(cfg.selection.retention) || cfg.selection.retention<=0 || cfg.selection.retention>1
        || !std::isfinite(cfg.selection.uniformMix) || cfg.selection.uniformMix<0 || cfg.selection.uniformMix>1
        || !std::isfinite(cfg.selection.spatialCellSize) || cfg.selection.spatialCellSize<0
        || (cfg.selection.selection!=PbvrSelection::Uniform && cfg.selection.selection!=PbvrSelection::Power
            && cfg.selection.selection!=PbvrSelection::SpatialPower)
        || (cfg.selection.selection==PbvrSelection::SpatialPower && cfg.selection.spatialCellSize<=0))
        return fail("Invalid depth photon transport settings");
    std::vector<V> normals; std::vector<double> areas,weights;
    double total=0;
    for(const auto& t:triangles) {
        const V a=vec(t.v0),b=vec(t.v1),c=vec(t.v2),albedo=vec(t.albedo),emission=vec(t.emission);
        const V cross=glm::cross(b-a,c-a); const double area=glm::length(cross)*0.5;
        if(!finite(a)||!finite(b)||!finite(c)||!finite(cross)||!std::isfinite(area)||area<1e-14
            || !finite(albedo)||std::min({albedo.x,albedo.y,albedo.z})<0||std::max({albedo.x,albedo.y,albedo.z})>1
            || !finite(emission)||std::min({emission.x,emission.y,emission.z})<0||t.metallic!=0
            || t.baseColorTextureIndex!=-1||t.normalTextureIndex!=-1||t.metallicRoughnessTextureIndex!=-1||t.emissiveTextureIndex!=-1)
            return fail("Depth transport supports finite diffuse untextured triangles only");
        normals.push_back(cross/(2*area)); areas.push_back(area);
        total+=area*glm::dot(emission,V(0.2126,0.7152,0.0722)); weights.push_back(total);
    }
    if(!std::isfinite(total)||total<=0) return fail("Scene needs emissive triangle power");
    std::mt19937 rng(cfg.randomSeed);
    const auto random=[&]() { return (static_cast<double>(rng())+0.5)/4294967296.0; };
    std::vector<Source> sources;
    for(int i=0;i<cfg.lightSamples;++i) {
        const auto it=std::upper_bound(weights.begin(),weights.end(),random()*total);
        const std::size_t id=std::min(static_cast<std::size_t>(it-weights.begin()),triangles.size()-1);
        const auto& t=triangles[id]; const double root=std::sqrt(random()),v=random();
        const double probability=(weights[id]-(id?weights[id-1]:0))/total;
        if(probability<=0) return fail("Light selection lost numerical support");
        const V position=vec(t.v0)*(1-root)+vec(t.v1)*(root*(1-v))+vec(t.v2)*(root*v);
        const V power=vec(t.emission)*(pi*areas[id]/(cfg.lightSamples*probability));
        if(!positive(power)) return fail("Invalid source flux");
        sources.push_back({position,normals[id],power});
    }
    std::vector<Photon> photons;
    for(int bounce=0;bounce<cfg.maxDepth && !sources.empty();++bounce) {
        DepthPhotonBounceStats step; step.sources=sources.size(); step.resolution=cfg.firstResolution;
        const int reductions=cfg.resolutionDropEvery?bounce/cfg.resolutionDropEvery:0;
        for(int r=0;r<reductions && step.resolution>cfg.minResolution;++r)
            step.resolution=std::max(cfg.minResolution,step.resolution/2);
        std::vector<PbvrArrival> candidates;
        for(const auto& source:sources) {
            step.launchedFlux+=source.power;
            const auto fs=faces(source.normal); const auto pixels=quadrature(fs,source.normal,step.resolution);
            const V eye=source.position+source.normal*cfg.sourceOffset;
            for(std::size_t f=0;f<fs.size();++f) {
                RtCameraSpec camera; camera.fovDeg=90;
                const V target=eye+fs[f].forward;
                for(int c=0;c<3;++c) { camera.lookFrom[c]=eye[c]; camera.lookAt[c]=target[c]; camera.up[c]=fs[f].up[c]; }
                PhotonGBuffer buffer;
                if(!gpu.rasterize(triangles,camera,step.resolution,step.resolution,buffer,cfg.nearPlane,cfg.farPlane))
                    return fail(gpu.getLastError());
                ++step.depthMaps;
                for(std::size_t i=0;i<buffer.receivers.size();++i) {
                    const auto& q=pixels[f*buffer.receivers.size()+i]; if(q.weight==0) continue;
                    const V power=source.power*q.weight; const auto& receiver=buffer.receivers[i];
                    if(receiver.triangle<0) { step.escapedFlux+=power; continue; }
                    if(static_cast<std::size_t>(receiver.triangle)>=triangles.size() || !positive(power))
                        return fail("Invalid depth arrival");
                    ++step.arrivals; step.arrivedFlux+=power;
                    if(bounce>0 || cfg.storeDirectPhotons) {
                        if(photons.size()>=cfg.maxStoredPhotons) return fail("Depth photon storage budget exceeded");
                        Photon p; p.position=receiver.position; p.normal=receiver.normal; p.direction=q.direction;
                        p.power=power; p.bounce=bounce;
                        p.contribution=bounce==0?PhotonContribution::Direct:PhotonContribution::Indirect;
                        photons.push_back(p);
                    }
                    const V reflected=power*vec(triangles[receiver.triangle].albedo);
                    if(bounce+1<cfg.maxDepth && positive(reflected)) {
                        if(candidates.size()>=cfg.maxStoredPhotons) return fail("Depth arrival budget exceeded");
                        candidates.push_back({receiver.position,reflected,receiver.triangle});
                    }
                }
            }
        }
        sources.clear();
        if(!candidates.empty()) {
            auto selection=cfg.selection;
            // Uniform uses the same fixed-budget stratified machinery, with q=1/M.
            if(selection.selection==PbvrSelection::Uniform) { selection.selection=PbvrSelection::Power; selection.uniformMix=1; }
            std::vector<PbvrSelectedSample> selected; PbvrResampleStats selectionStats;
            if(!resamplePbvr(candidates,selection,cfg.randomSeed^cfg.selection.randomSeed^
                (static_cast<std::uint32_t>(bounce+1)*0x9e3779b9u),selected,selectionStats,error_)) return fail(error_);
            for(const auto& sample:selected) {
                const auto& arrival=candidates[sample.source]; const V power=arrival.power*sample.powerScale;
                if(!positive(power)) return fail("Invalid corrected depth source flux");
                sources.push_back({arrival.position,normals[arrival.surface],power});
            }
        }
        step.retained=sources.size();
        if(!finite(step.launchedFlux)||!finite(step.arrivedFlux)||!finite(step.escapedFlux)) return fail("Depth flux overflow");
        stats_.bounces.push_back(step);
    }
    if(!output.build(std::move(photons),cfg.buildPhotonIndex)) return fail("Invalid depth photon map");
    stats_.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    return true;
}
} // namespace Phantom::RayTracer
