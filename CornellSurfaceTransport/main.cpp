// CPU surface counterpart of PBVR's particle-probe scattering iteration.
// Geometry is intersected exactly; particles cache outgoing diffuse radiance.
#include "../../CGLib/Math/Vector3d.h"
#include "../RayTracer/PhotonMapper.h"
#ifdef PHOTON_SPLAT_VULKAN
#include "../PhotonSplatGpu/PhotonSplatGpu.h"
#include "../../CGLib/VulkanGraphics/VulkanContext.h"
#include "../../CGLib/VulkanGraphics/VulkanCommandPool.h"
#endif
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {
using V = Phantom::Math::Vector3dd;
constexpr double pi = 3.14159265358979323846;
constexpr double epsilon = 1e-6;
struct Random {
    std::mt19937 engine;
    explicit Random(std::uint32_t seed) : engine(seed) {}
    double uniform() { return (static_cast<double>(engine()) + 0.5) / 4294967296.0; }
};
struct Quad { V origin, u, v, normal, reflectance, emission; };
struct Hit { int face = -1; double distance = 1e30, u = 0, v = 0; V position{0}; };
using Scene = std::vector<Quad>;
using Field = std::vector<std::vector<V>>;
bool emitting(const Quad& q) { return glm::dot(q.emission, q.emission) > 0; }

Hit intersect(const Scene& scene, const V& origin, const V& direction, double limit = 1e30)
{
    Hit result; result.distance = limit;
    for (std::size_t i = 0; i < scene.size(); ++i) {
        const auto& q = scene[i];
        const double denominator = glm::dot(direction, q.normal);
        if (std::abs(denominator) < 1e-12) continue;
        const double t = glm::dot(q.origin - origin, q.normal) / denominator;
        if (t <= epsilon || t >= result.distance) continue;
        const V p = origin + direction * t, delta = p - q.origin;
        const double u = glm::dot(delta, q.u) / glm::dot(q.u, q.u);
        const double v = glm::dot(delta, q.v) / glm::dot(q.v, q.v);
        if (u < 0 || u > 1 || v < 0 || v > 1) continue;
        result = {static_cast<int>(i), t, u, v, p};
    }
    return result;
}
void addQuad(Scene& s, V o, V u, V v, V n, V color, V emission = V(0))
{ s.push_back({o, u, v, n, color, emission}); }
void addBox(Scene& s, V lo, V hi)
{
    const V d = hi - lo, white(0.73);
    addQuad(s, lo, V(d.x,0,0), V(0,d.y,0), V(0,0,-1), white);
    addQuad(s, V(lo.x,lo.y,hi.z), V(d.x,0,0), V(0,d.y,0), V(0,0,1), white);
    addQuad(s, lo, V(0,d.y,0), V(0,0,d.z), V(-1,0,0), white);
    addQuad(s, V(hi.x,lo.y,lo.z), V(0,d.y,0), V(0,0,d.z), V(1,0,0), white);
    addQuad(s, V(lo.x,hi.y,lo.z), V(d.x,0,0), V(0,0,d.z), V(0,1,0), white);
    // Bottom coincides with the floor and is intentionally omitted.
}
Scene cornell()
{
    Scene s; const V white(0.73);
    addQuad(s, V(0,0,0), V(0,2,0), V(0,0,2), V(1,0,0), V(0.65,0.05,0.05));
    addQuad(s, V(2,0,0), V(0,2,0), V(0,0,2), V(-1,0,0), V(0.12,0.45,0.15));
    addQuad(s, V(0,0,0), V(2,0,0), V(0,0,2), V(0,1,0), white);
    addQuad(s, V(0,2,0), V(2,0,0), V(0,0,2), V(0,-1,0), white);
    addQuad(s, V(0,0,2), V(2,0,0), V(0,2,0), V(0,0,-1), white);
    addBox(s, V(0.25,0,0.65), V(0.85,0.6,1.25));
    addBox(s, V(1.15,0,1.1), V(1.75,1.2,1.7));
    addQuad(s, V(0.7,1.999,0.7), V(0.6,0,0), V(0,0,0.6), V(0,-1,0), V(0), V(12));
    return s;
}
V cosineDirection(V normal, Random& rng)
{
    const double r = std::sqrt(rng.uniform()), angle = 2*pi*rng.uniform();
    const V tangent = glm::normalize(glm::cross(std::abs(normal.y) < 0.9 ? V(0,1,0) : V(1,0,0), normal));
    return tangent*(r*std::cos(angle)) + glm::cross(normal,tangent)*(r*std::sin(angle))
        + normal*std::sqrt(std::max(0.0,1-r*r));
}
V direct(const Scene& s, const Hit& h, int samples, Random& rng)
{
    const Quad& receiver = s[h.face]; V sum(0);
    for (const auto& light : s) {
        if (!emitting(light)) continue;
        const double area = glm::length(glm::cross(light.u,light.v));
        for (int k=0; k<samples; ++k) {
            const V target = light.origin + light.u*rng.uniform() + light.v*rng.uniform();
            const V origin = h.position + receiver.normal*epsilon*4.0;
            const V delta = target-origin; const double distance = glm::length(delta);
            const V direction = delta/distance;
            const double geometry = std::max(0.0,glm::dot(receiver.normal,direction))
                * std::max(0.0,glm::dot(light.normal,-direction)) / (distance*distance);
            if (geometry == 0 || intersect(s,origin,direction,distance-epsilon*2).face >= 0) continue;
            sum += receiver.reflectance * light.emission * (area*geometry/(pi*samples));
        }
    }
    return sum;
}
Field emptyField(const Scene& scene, int grid)
{ return Field(scene.size(), std::vector<V>(static_cast<std::size_t>(grid)*grid,V(0))); }
V lookup(const Field& field, const Hit& h, int grid)
{
    // Per-face interpolation cannot leak through a nearby wall or another box.
    const double x = std::clamp(h.u*grid-0.5,0.0,static_cast<double>(grid-1));
    const double y = std::clamp(h.v*grid-0.5,0.0,static_cast<double>(grid-1));
    const int ix=static_cast<int>(x), iy=static_cast<int>(y), jx=std::min(ix+1,grid-1), jy=std::min(iy+1,grid-1);
    const double a=x-ix,b=y-iy; const auto& f=field[h.face];
    return f[iy*grid+ix]*((1-a)*(1-b)) + f[iy*grid+jx]*(a*(1-b))
         + f[jy*grid+ix]*((1-a)*b) + f[jy*grid+jx]*(a*b);
}
Hit particle(const Scene& scene, int face, int index, int grid)
{
    const double u=(index%grid+0.5)/grid, v=(index/grid+0.5)/grid;
    return {face,0,u,v,scene[face].origin+scene[face].u*u+scene[face].v*v};
}
Field solve(const Scene& scene, int grid, int samples, int orders, std::uint32_t seed, bool reduce)
{
    Field previous=emptyField(scene,grid), indirect=emptyField(scene,grid);
    for (int face=0; face<static_cast<int>(scene.size()); ++face) {
        if (emitting(scene[face])) continue;
        for (int i=0; i<grid*grid; ++i) {
            Random rng(seed+static_cast<std::uint32_t>(face*grid*grid+i));
            previous[face][i]=direct(scene,particle(scene,face,i,grid),samples,rng);
        }
    }
    for (int order=0; order<orders; ++order) {
        const int budget=reduce ? std::max(4,samples/(1<<std::min(order,12))) : samples;
        Field next=emptyField(scene,grid);
        for (int face=0; face<static_cast<int>(scene.size()); ++face) {
            const auto& q=scene[face]; if (emitting(q)) continue;
            for (int i=0; i<grid*grid; ++i) {
                Random rng(seed+0x9e3779b9u*static_cast<std::uint32_t>(order+1)
                    +static_cast<std::uint32_t>(face*grid*grid+i));
                const Hit p=particle(scene,face,i,grid); V sum(0);
                for (int k=0; k<budget; ++k) {
                    const V d=cosineDirection(q.normal,rng);
                    const Hit h=intersect(scene,p.position+q.normal*epsilon*4.0,d);
                    if (h.face>=0 && glm::dot(scene[h.face].normal,-d)>0 && !emitting(scene[h.face]))
                        sum+=lookup(previous,h,grid);
                }
                // Cosine-weighted PDF cancels Lambertian cosine / pi.
                next[face][i]=q.reflectance*sum/static_cast<double>(budget);
                indirect[face][i]+=next[face][i];
            }
        }
        previous=std::move(next);
        std::cout << "indirect order " << order+1 << ": " << budget << " rays/particle\n";
    }
    return indirect;
}
// Separate stochastic path integrator: no particle cache or interpolation.
V trace(const Scene& scene, V origin, V direction, int depth, Random& rng)
{
    V sum(0), throughput(1);
    for (int bounce=0; bounce<depth; ++bounce) {
        const Hit h=intersect(scene,origin,direction); if(h.face<0) break;
        const auto& q=scene[h.face]; if(glm::dot(q.normal,-direction)<=0) break;
        if(emitting(q)) { if(bounce==0) sum+=throughput*q.emission; break; }
        sum+=throughput*direct(scene,h,1,rng);
        throughput*=q.reflectance;
        origin=h.position+q.normal*epsilon*4.0; direction=cosineDirection(q.normal,rng);
    }
    return sum;
}
V cameraRay(double x,double y,int size)
{
    const double scale=std::tan(38.0*pi/360.0);
    return glm::normalize(V((2*x/size-1)*scale,(1-2*y/size)*scale,1));
}
std::vector<V> render(const Scene& scene, const Field* field, int grid, int size, int spp,
                      int depth, std::uint32_t seed, bool directOnly=false)
{
    std::vector<V> image(static_cast<std::size_t>(size)*size,V(0)); const V eye(1,1,-3.2);
    for(int y=0;y<size;++y) for(int x=0;x<size;++x) {
        Random rng(seed+static_cast<std::uint32_t>(y*size+x)); V sum(0);
        for(int k=0;k<spp;++k) {
            const V d=cameraRay(x+rng.uniform(),y+rng.uniform(),size);
            if(!field && !directOnly) { sum+=trace(scene,eye,d,depth,rng); continue; }
            const Hit h=intersect(scene,eye,d); if(h.face<0) continue;
            if(glm::dot(scene[h.face].normal,-d)<=0) continue;
            if(emitting(scene[h.face])) sum+=scene[h.face].emission;
            else { sum+=direct(scene,h,1,rng); if(field) sum+=lookup(*field,h,grid); }
        }
        image[y*size+x]=sum/static_cast<double>(spp);
    }
    return image;
}
bool writeImage(const std::filesystem::path& path, const std::vector<V>& image, int size)
{
    std::ofstream ppm(path.string()+".ppm",std::ios::binary);
    std::ofstream pfm(path.string()+".pfm",std::ios::binary);
    if(!ppm || !pfm) return false;
    ppm<<"P6\n"<<size<<' '<<size<<"\n255\n";
    const std::uint16_t endian=1;
    pfm<<"PF\n"<<size<<' '<<size<<'\n'<<(*reinterpret_cast<const unsigned char*>(&endian) ? "-1.0\n" : "1.0\n");
    for(const auto& p:image) for(int c=0;c<3;++c) {
        const double mapped=std::max(0.0,p[c])/(1+std::max(0.0,p[c]));
        const unsigned char value=static_cast<unsigned char>(255*std::pow(mapped,1/2.2)+0.5);
        ppm.write(reinterpret_cast<const char*>(&value),1);
    }
    for(int y=size-1;y>=0;--y) for(int x=0;x<size;++x) for(int c=0;c<3;++c) {
        const float value=static_cast<float>(image[y*size+x][c]);
        pfm.write(reinterpret_cast<const char*>(&value),sizeof(value));
    }
    return ppm.good() && pfm.good();
}
double seconds(std::chrono::steady_clock::time_point start)
{ return std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count(); }
bool checks()
{
    const Scene scene=cornell();
    const Hit wall=intersect(scene,V(1,1,-3.2),V(0,0,1));
    if(wall.face!=4) return false;
    const Hit blocked=intersect(scene,V(0.5,0.3,0.1),V(0,0,1));
    if(blocked.face!=5) return false;
    Random rng(42); double mean=0;
    for(int i=0;i<10000;++i) { const V d=cosineDirection(V(0,1,0),rng); if(d.y<0 || std::abs(glm::length(d)-1)>1e-10) return false; mean+=d.y; }
    if(std::abs(mean/10000-2.0/3.0)>0.02) return false;
    Field field=emptyField(scene,2);
    for(auto& v:field[0]) v=V(1,0,0);
    for(auto& v:field[1]) v=V(0,1,0);
    if(glm::length(lookup(field,particle(scene,0,0,2),2)-V(1,0,0))>1e-12) return false;
    Scene black=scene; for(auto& q:black) q.reflectance=V(0);
    const Field result=solve(black,2,4,2,42,true);
    for(const auto& face:result) for(const auto& p:face) if(glm::length(p)>1e-12) return false;
    Scene dark=scene; for(auto& q:dark) q.emission=V(0);
    const Field darkResult=solve(dark,2,4,2,42,true);
    for(const auto& face:darkResult) for(const auto& p:face) if(glm::length(p)>1e-12) return false;
    const Field zeroOrder=solve(scene,2,4,0,42,true);
    for(const auto& face:zeroOrder) for(const auto& p:face) if(glm::length(p)>1e-12) return false;
    const Field illuminated=solve(scene,4,64,1,42,true);
    double ceilingIndirect=0;
    for(const auto& p:illuminated[3]) ceilingIndirect+=p.x+p.y+p.z;
    if(ceilingIndirect<=0) return false; // Ceiling cannot see the downward-facing lamp directly.
    return true;
}
#ifdef PHOTON_SPLAT_VULKAN
int comparePhotonSplat(Phantom::RayTracer::PhotonMapper& mapper,
    const std::vector<Phantom::RayTracer::RtTriangle>& triangles,
    const Phantom::RayTracer::RtCameraSpec& camera,int size,double radius,
    const std::filesystem::path& out,const std::filesystem::path& executable,int seed,int directSamples)
{
    namespace RT=Phantom::RayTracer;
    Phantom::VKG::VulkanContext context; Phantom::VKG::VulkanCommandPool pool;
    struct PoolCleanup { Phantom::VKG::VulkanCommandPool& pool; ~PoolCleanup() { pool.destroy(); } } cleanup{pool};
    if(!context.createInstance("Photon depth-buffer splatting",{},true) || !context.initDevice(VK_NULL_HANDLE)
        || !pool.init(&context,VK_NULL_HANDLE)) return 1;
    // Explicit scope preserves the borrowed context/pool lifetime on every exit.
    int result=1;
    {
        RT::PhotonSplatGpu gpu;
        if(!gpu.create(context,pool,(executable.parent_path()/"photon_splat_shaders").string())) { std::cerr<<gpu.getLastError()<<'\n'; return 1; }
        Phantom::Graphics::Imagef gpuIndirect; RT::PhotonGBuffer gbuffer;
        const auto gpuStart=std::chrono::steady_clock::now();
        if(!gpu.render(triangles,mapper.getPhotonMap(),camera,size,size,radius,gpuIndirect,gbuffer,
            RT::PhotonContribution::Indirect,0.1,100)) { std::cerr<<gpu.getLastError()<<'\n'; return 1; }
        const double gpuWall=seconds(gpuStart);
        Phantom::Graphics::Imagef kdIndirect(size,size); const auto kdStart=std::chrono::steady_clock::now();
        for(int y=0;y<size;++y) for(int x=0;x<size;++x) {
            const auto& receiver=gbuffer.receivers[y*size+x]; V color(0);
            if(receiver.triangle>=0) {
                const auto& t=triangles[receiver.triangle];
                color=mapper.getPhotonMap().estimateRadiance(receiver.position,receiver.normal,
                    V(t.albedo[0],t.albedo[1],t.albedo[2]),radius,RT::PhotonContribution::Indirect);
            }
            kdIndirect.setColor(x,y,Phantom::Graphics::ColorRGBAf(static_cast<float>(color.x),static_cast<float>(color.y),static_cast<float>(color.z),1));
        }
        const double kdWall=seconds(kdStart);
        double squared=0,referenceSquared=0,maximumError=0; std::size_t valid=0;
        for(int y=0;y<size;++y) for(int x=0;x<size;++x) {
            const auto a=gpuIndirect.getColor(x,y),b=kdIndirect.getColor(x,y);
            if(gbuffer.receivers[y*size+x].triangle>=0) ++valid;
            for(int c=0;c<3;++c) { const double delta=static_cast<double>(a[c])-b[c]; squared+=delta*delta; referenceSquared+=static_cast<double>(b[c])*b[c]; maximumError=std::max(maximumError,std::abs(delta)); }
        }
        Phantom::Graphics::Imagef gpuImage,kdImage;
        const auto shadeStart=std::chrono::steady_clock::now();
        if(!mapper.shadeGBuffer(gbuffer,gpuImage,&gpuIndirect) || !mapper.shadeGBuffer(gbuffer,kdImage,&kdIndirect)) return 1;
        const double shadeWall=seconds(shadeStart);
        const auto write=[&](const char* name,const Phantom::Graphics::Imagef& image) {
            std::vector<V> pixels; pixels.reserve(static_cast<std::size_t>(size)*size);
            for(int y=0;y<size;++y) for(int x=size-1;x>=0;--x) { const auto c=image.getColor(x,y); pixels.emplace_back(c.x,c.y,c.z); }
            return writeImage(out/name,pixels,size);
        };
        std::error_code error; std::filesystem::create_directories(out,error);
        if(error || !write("splat",gpuImage) || !write("kdtree",kdImage)
            || !write("splat_indirect",gpuIndirect) || !write("kdtree_indirect",kdIndirect)) return 1;
        const auto& stats=gpu.getStats(); std::ofstream csv(out/"splat_metrics.csv");
        const double relative=std::sqrt(squared/std::max(referenceSquared,1e-30));
        csv<<"device,size,photon_count,radius,seed,direct_samples,receivers,splatted_photons,photon_build_seconds,gpu_wall_seconds,upload_seconds,submit_readback_seconds,gbuffer_gpu_ms,splat_gpu_ms,copy_gpu_ms,kdtree_gather_seconds,two_composites_seconds,indirect_relative_l2,max_absolute_error\n";
        csv<<'"'<<context.getDeviceName()<<'"'<<','<<size<<','<<mapper.getStats().emittedPhotons<<','<<radius<<','<<seed<<','<<directSamples<<','<<valid<<','<<stats.splattedPhotons<<','<<mapper.getStats().buildSeconds<<','<<gpuWall<<','<<stats.uploadSeconds<<','<<stats.submitReadbackSeconds<<','<<stats.gbufferGpuMilliseconds<<','<<stats.splatGpuMilliseconds<<','<<stats.copyGpuMilliseconds<<','<<kdWall<<','<<shadeWall<<','<<relative<<','<<maximumError<<'\n';
        std::cout<<"GPU splat wall="<<gpuWall<<"s GPU pass="<<stats.splatGpuMilliseconds<<"ms KD gather="<<kdWall<<"s relative L2="<<relative<<" max error="<<maximumError<<'\n';
        // Gross mismatches fail the experiment; small float differences are expected.
        result=csv.good() && valid>0 && relative<0.01?0:1;
    }
    return result;
}
#endif
int runPhotonMapping(int argc,char** argv)
{
    // --photon out size photonCount gatherRadius spp seed
    if(argc!=8) { std::cerr<<"Usage: CornellSurfaceTransport --photon out size photonCount gatherRadius spp seed\n"; return 1; }
    const auto integer=[](const char* text,int maximum) {
        char* end=nullptr; const long value=std::strtol(text,&end,10);
        return end!=text && *end=='\0' && value>=0 && value<=maximum?static_cast<int>(value):-1;
    };
    const int size=integer(argv[3],2048),count=integer(argv[4],10000000),spp=integer(argv[6],65536),seed=integer(argv[7],1000000000);
    char* end=nullptr; const double radius=std::strtod(argv[5],&end);
    if(size<1 || count<1 || spp<1 || seed<0 || end==argv[5] || *end!='\0' || !std::isfinite(radius) || radius<=0) return 1;
    std::vector<Phantom::RayTracer::RtTriangle> triangles;
    for(const auto& q:cornell()) {
        V u=q.u,v=q.v;
        if(glm::dot(glm::cross(u,v),q.normal)<0) std::swap(u,v);
        const V vertices[]={q.origin,q.origin+u,q.origin+u+v,q.origin+v};
        for(const auto& indices:{std::vector<int>{0,1,2},std::vector<int>{0,2,3}}) {
            Phantom::RayTracer::RtTriangle t; t.roughness=1;
            for(int c=0;c<3;++c) {
                t.v0[c]=vertices[indices[0]][c]; t.v1[c]=vertices[indices[1]][c]; t.v2[c]=vertices[indices[2]][c];
                t.albedo[c]=q.reflectance[c]; t.emission[c]=q.emission[c];
            }
            triangles.push_back(t);
        }
    }
    Phantom::RayTracer::PhotonMappingSettings cfg;
    const bool splat=std::string(argv[1])=="--photon-splat";
    cfg.photonCount=static_cast<std::size_t>(count); cfg.gatherRadius=radius;
    cfg.render.width=size; cfg.render.height=size; cfg.render.samplesPerPixel=spp;
    cfg.render.randomSeed=static_cast<std::uint32_t>(seed); cfg.photonMaxDepth=8;
    if(splat) cfg.directLightSamples=spp;
    Phantom::RayTracer::PhotonMapper mapper(cfg);
    if(!mapper.build(triangles)) { std::cerr<<mapper.getLastError()<<'\n'; return 1; }
    Phantom::RayTracer::RtCameraSpec cam;
    cam.lookFrom[0]=cam.lookFrom[1]=1; cam.lookFrom[2]=-3.2;
    cam.lookAt[0]=cam.lookAt[1]=1; cam.lookAt[2]=1; cam.fovDeg=38;
    if(splat) {
#ifdef PHOTON_SPLAT_VULKAN
        std::error_code error; const auto executable=std::filesystem::absolute(argv[0],error);
        if(error) return 1;
        return comparePhotonSplat(mapper,triangles,cam,size,radius,argv[2],executable,seed,spp);
#else
        std::cerr<<"This build has no Vulkan photon-splat backend\n"; return 1;
#endif
    }
    Phantom::Graphics::Imagef linear; const auto start=std::chrono::steady_clock::now();
    if(!mapper.renderLinear(cam,linear)) return 1;
    const double renderSeconds=seconds(start);
    std::vector<V> pixels; pixels.reserve(static_cast<std::size_t>(size)*size);
    // Match the historical experiment's +x screen axis for side-by-side images.
    for(int y=0;y<size;++y) for(int x=size-1;x>=0;--x) { const auto p=linear.getColor(x,y); pixels.emplace_back(p.x,p.y,p.z); }
    const std::filesystem::path out=argv[2]; std::error_code error;
    std::filesystem::create_directories(out,error); if(error || !writeImage(out/"photon",pixels,size)) return 1;
    const auto& stats=mapper.getStats(); std::ofstream csv(out/"photon_metrics.csv");
    csv<<"size,photon_count,gather_radius,spp,seed,build_seconds,render_seconds,direct_photons,indirect_photons,caustic_photons\n";
    csv<<size<<','<<count<<','<<radius<<','<<spp<<','<<seed<<','<<stats.buildSeconds<<','<<renderSeconds<<','<<stats.directPhotons<<','<<stats.indirectPhotons<<','<<stats.causticPhotons<<'\n';
    std::cout<<"photon build="<<stats.buildSeconds<<"s render="<<renderSeconds<<"s stored="<<mapper.getPhotonMap().getPhotons().size()<<'\n';
    return csv.good()?0:1;
}
}
int main(int argc,char** argv)
{
    if(argc>1 && (std::string(argv[1])=="--photon" || std::string(argv[1])=="--photon-splat")) return runPhotonMapping(argc,argv);
    if(argc==2 && std::string(argv[1])=="--check") { const bool ok=checks(); std::cout<<(ok?"Checks passed\n":"Checks failed\n"); return ok?0:1; }
    // Positional parameters keep the experiment reproducible without JSON dependencies.
    if(argc>10) { std::cerr<<"Usage: CornellSurfaceTransport [out size grid samples orders referenceSpp seed reduce displaySpp]\n"; return 1; }
    const auto number=[&](int index,int fallback) { if(argc<=index) return fallback; char* end=nullptr; const long n=std::strtol(argv[index],&end,10); return end!=argv[index] && *end=='\0' && n>=0 && n<=100000 ? static_cast<int>(n) : -1; };
    const std::filesystem::path out=argc>1?argv[1]:"results";
    const int size=number(2,128),grid=number(3,16),samples=number(4,64),orders=number(5,3),refSpp=number(6,256),seed=number(7,42),reduce=number(8,1),displaySpp=number(9,64);
    if(size<1 || size>2048 || grid<1 || grid>128 || samples<1 || orders<0 || orders>12 || refSpp<1 || seed<0 || reduce<0 || reduce>1 || displaySpp<1) { std::cerr<<"Invalid parameters\n"; return 1; }
    std::error_code error; std::filesystem::create_directories(out,error); if(error) { std::cerr<<error.message()<<'\n'; return 1; }
    const Scene scene=cornell(); const auto begin=std::chrono::steady_clock::now();
    const Field field=solve(scene,grid,samples,orders,static_cast<std::uint32_t>(seed),reduce!=0);
    const double solveTime=seconds(begin); auto start=std::chrono::steady_clock::now();
    const auto cached=render(scene,&field,grid,size,displaySpp,orders+1,static_cast<std::uint32_t>(seed));
    const double renderTime=seconds(start); start=std::chrono::steady_clock::now();
    const auto reference=render(scene,nullptr,grid,size,refSpp,orders+1,static_cast<std::uint32_t>(seed)^0xa511e9b3u);
    const double referenceTime=seconds(start);
    const auto directImage=render(scene,nullptr,grid,size,displaySpp,1,static_cast<std::uint32_t>(seed),true);
    double squared=0,referenceSquared=0,mean=0,referenceMean=0,indirectMean=0;
    double diffuseSquared=0,diffuseReferenceSquared=0; std::size_t diffusePixels=0;
    for(std::size_t i=0;i<cached.size();++i) {
        const V delta=cached[i]-reference[i]; squared+=glm::dot(delta,delta);
        referenceSquared+=glm::dot(reference[i],reference[i]);
        const Hit center=intersect(scene,V(1,1,-3.2),cameraRay(i%size+0.5,i/size+0.5,size));
        bool interior=center.face>=0 && !emitting(scene[center.face]);
        for(double dy : {-0.5,0.5}) for(double dx : {-0.5,0.5}) {
            const Hit corner=intersect(scene,V(1,1,-3.2),cameraRay(i%size+0.5+dx,i/size+0.5+dy,size));
            interior=interior && corner.face==center.face;
        }
        if(interior) {
            diffuseSquared+=glm::dot(delta,delta);
            diffuseReferenceSquared+=glm::dot(reference[i],reference[i]); ++diffusePixels;
        }
        mean+=(cached[i].x+cached[i].y+cached[i].z)/3;
        referenceMean+=(reference[i].x+reference[i].y+reference[i].z)/3;
        const V indirect=cached[i]-directImage[i]; indirectMean+=(indirect.x+indirect.y+indirect.z)/3;
        for(int c=0;c<3;++c) if(!std::isfinite(cached[i][c]) || cached[i][c]<0) { std::cerr<<"Invalid radiance\n"; return 1; }
    }
    if(!writeImage(out/"surface",cached,size) || !writeImage(out/"reference",reference,size) || !writeImage(out/"direct",directImage,size)) return 1;
    std::ofstream metrics(out/"metrics.csv");
    metrics<<"size,grid,samples,orders,reference_spp,seed,reduce,display_spp,solve_seconds,render_seconds,reference_seconds,linear_rmse,relative_l2,mean_rgb,reference_mean_rgb,indirect_mean_rgb,diffuse_pixels,diffuse_relative_l2\n";
    metrics<<size<<','<<grid<<','<<samples<<','<<orders<<','<<refSpp<<','<<seed<<','<<reduce<<','<<displaySpp<<','<<solveTime<<','<<renderTime<<','<<referenceTime<<','<<std::sqrt(squared/(3*cached.size()))<<','<<std::sqrt(squared/std::max(referenceSquared,1e-30))<<','<<mean/cached.size()<<','<<referenceMean/cached.size()<<','<<indirectMean/cached.size()<<','<<diffusePixels<<','<<std::sqrt(diffuseSquared/std::max(diffuseReferenceSquared,1e-30))<<'\n';
    std::cout<<"solve="<<solveTime<<"s render="<<renderTime<<"s reference="<<referenceTime<<"s relative L2="<<std::sqrt(squared/std::max(referenceSquared,1e-30))<<'\n';
    return metrics.good()?0:1;
}
