#include "pch.h"
#include "PbvrPreviewPanel.h"
#include "../PhotonSplatGpu/DepthPhotonTransport.h"
#include "../../CGLib/VulkanGraphics/VulkanContext.h"
#include "../../CGLib/VulkanGraphics/VulkanCommandPool.h"
#include "../../CGLib/VulkanGraphics/VulkanSPVResolver.h"
#include "../../CGLib/VkAppBase/ScenarioRunner/ViewShell.h"
#include "../../CGLib/Graphics/ImageFileWriter.h"
#include <fstream>

namespace {
namespace RT=Phantom::RayTracer;
using V=Phantom::Math::Vector3dd;
constexpr int edge=96;
constexpr double pi=3.14159265358979323846;
std::vector<RT::RtTriangle> cornell() {
    std::vector<RT::RtTriangle> result;
    const auto quad=[&](V origin,V u,V v,V normal,V color,V emission=V(0)) {
        if(glm::dot(glm::cross(u,v),normal)<0) std::swap(u,v);
        const V p[]={origin,origin+u,origin+u+v,origin+v};
        for(const auto& ids:{std::vector<int>{0,1,2},std::vector<int>{0,2,3}}) {
            RT::RtTriangle t;
            for(int c=0;c<3;++c) { t.v0[c]=p[ids[0]][c]; t.v1[c]=p[ids[1]][c]; t.v2[c]=p[ids[2]][c]; t.albedo[c]=color[c]; t.emission[c]=emission[c]; }
            result.push_back(t);
        }
    };
    const V white(0.73);
    quad(V(0),V(0,2,0),V(0,0,2),V(1,0,0),V(0.65,0.05,0.05));
    quad(V(2,0,0),V(0,2,0),V(0,0,2),V(-1,0,0),V(0.12,0.45,0.15));
    quad(V(0),V(2,0,0),V(0,0,2),V(0,1,0),white);
    quad(V(0,2,0),V(2,0,0),V(0,0,2),V(0,-1,0),white);
    quad(V(0,0,2),V(2,0,0),V(0,2,0),V(0,0,-1),white);
    for(const auto& bounds:std::vector<std::pair<V,V>>{{V(0.25,0,0.65),V(0.85,0.6,1.25)},{V(1.15,0,1.1),V(1.75,1.2,1.7)}}) {
        const V lo=bounds.first,hi=bounds.second,d=hi-lo;
        quad(lo,V(d.x,0,0),V(0,d.y,0),V(0,0,-1),white);
        quad(V(lo.x,lo.y,hi.z),V(d.x,0,0),V(0,d.y,0),V(0,0,1),white);
        quad(lo,V(0,d.y,0),V(0,0,d.z),V(-1,0,0),white);
        quad(V(hi.x,lo.y,lo.z),V(0,d.y,0),V(0,0,d.z),V(1,0,0),white);
        quad(V(lo.x,hi.y,lo.z),V(d.x,0,0),V(0,0,d.z),V(0,1,0),white);
    }
    quad(V(0.7,1.999,0.7),V(0.6,0,0),V(0,0,0.6),V(0,-1,0),V(0),V(12));
    return result;
}
Phantom::Graphics::Imageuc display(const std::vector<V>& values,const Phantom::Graphics::Imagef& direct) {
    Phantom::Graphics::Imageuc image(edge,edge);
    for(int y=0;y<edge;++y) for(int x=0;x<edge;++x) {
        const auto d=direct.getColor(x,y); const V value=values[y*edge+x]+V(d.x,d.y,d.z);
        unsigned char rgb[3];
        for(int c=0;c<3;++c) { const double v=std::max(0.0,value[c]); rgb[c]=static_cast<unsigned char>(std::lround(255*std::pow(v/(1+v),1/2.2))); }
        image.setColor(x,y,Phantom::Graphics::ColorRGBAuc(rgb[0],rgb[1],rgb[2],255));
    }
    return image;
}
bool pfm(const std::filesystem::path& path,const std::vector<V>& values) {
    std::ofstream file(path,std::ios::binary); file<<"PF\n"<<edge<<' '<<edge<<"\n-1.0\n";
    for(int y=edge-1;y>=0;--y) for(int x=0;x<edge;++x) {
        const V p=values[y*edge+x]; const float rgb[]={static_cast<float>(p.x),static_cast<float>(p.y),static_cast<float>(p.z)};
        file.write(reinterpret_cast<const char*>(rgb),sizeof(rgb));
    }
    return file.good();
}
}
struct PbvrPreviewPanel::Runtime {
    Phantom::VKG::VulkanContext context;
    Phantom::VKG::VulkanCommandPool pool;
    RT::PhotonSplatGpu gpu;
    const std::vector<RT::RtTriangle> scene=cornell();
    Phantom::Graphics::Imagef direct;
    std::uint64_t directGeneration=0;
    bool ready=false;
    ~Runtime() { gpu.destroy(); pool.destroy(); }
    bool create(const std::string& shaders) {
        if(!context.createInstance("PBVR progressive preview",{},true))
            if(!context.createInstance("PBVR progressive preview",{},false)) return false;
        ready=context.initDevice(VK_NULL_HANDLE)&&pool.init(&context,VK_NULL_HANDLE)&&gpu.create(context,pool,shaders);
        return ready;
    }
};
PbvrPreviewPanel::PbvrPreviewPanel()=default;
PbvrPreviewPanel::~PbvrPreviewPanel() { if(future_.valid()) future_.wait(); }
void PbvrPreviewPanel::init(Phantom::VKG::VulkanContext& context,const Phantom::VKG::VulkanCommandPool& pool,ViewShell& shell) {
    shell_=&shell; image_.init(&context,&pool);
    shaderDirectory_=(Phantom::VKG::detail::detectModuleDir()/"photon_splat_shaders").string();
}
void PbvrPreviewPanel::cleanup(VkDevice device) {
    enabled_=false; if(future_.valid()) { future_.wait(); future_.get(); }
    runtime_.reset(); image_.cleanup(device);
}
void PbvrPreviewPanel::reset() {
    ++generation_; sampleCount_=referenceCount_=0; coarse_=true; reference_=false;
    mean_.clear(); m2_.clear(); referenceSum_.clear(); standardError_=-1; error_.clear();
    lastChange_=std::chrono::steady_clock::now();
}
void PbvrPreviewPanel::start() {
    enabled_=true; reset(); startTime_=lastUpdate_=std::chrono::steady_clock::now();
    if(shell_) shell_->setPanelVisible("PBVR Progressive Preview",true);
}
bool PbvrPreviewPanel::startDemo(const std::filesystem::path& directory) {
    if(demo_ || directory.empty()) return false;
    std::error_code ec; std::filesystem::create_directories(directory,ec); if(ec) return false;
    demoDirectory_=directory; demoResponse_.reset(); metrics_.clear(); frameSeconds_.clear();
    firstImageSeconds_=refine32Seconds_=-1;
    yaw_=pitch_=0; distance_=4.2f; maxSamples_=32; demo_=true; start(); return true;
}
bool PbvrPreviewPanel::setYaw(float degrees) {
    if(demo_ || !std::isfinite(degrees) || std::abs(degrees)>75) return false;
    yaw_=degrees; start(); return true;
}
std::optional<std::string> PbvrPreviewPanel::takeDemoResponse() {
    auto response=std::move(demoResponse_); demoResponse_.reset(); return response;
}
void PbvrPreviewPanel::launch() {
    const auto generation=generation_; const bool coarse=coarse_,reference=reference_;
    const int sample=reference?referenceCount_:sampleCount_;
    RT::RtCameraSpec camera; const double yaw=yaw_*pi/180,pitch=pitch_*pi/180;
    const V target(1,1,1),eye=target+static_cast<double>(distance_)*V(std::sin(yaw)*std::cos(pitch),std::sin(pitch),-std::cos(yaw)*std::cos(pitch));
    for(int c=0;c<3;++c) { camera.lookFrom[c]=eye[c]; camera.lookAt[c]=target[c]; }
    camera.fovDeg=38;
    future_=std::async(std::launch::async,[this,generation,coarse,reference,sample,camera] {
        Pass result; result.generation=generation; result.coarse=coarse; result.reference=reference;
        const auto start=std::chrono::steady_clock::now();
        if(!runtime_) runtime_=std::make_unique<Runtime>();
        auto& r=*runtime_;
        if(!r.ready && !r.create(shaderDirectory_)) { result.error="Offscreen Vulkan initialization failed"; return result; }
        RT::DepthPhotonSettings settings; settings.projection=RT::DepthPhotonProjection::Paraboloid;
        settings.firstResolution=coarse?16:64; settings.minResolution=coarse?8:16;
        settings.lightSamples=coarse?2:8; settings.maxDepth=coarse?3:4;
        settings.selection.maxParticles=coarse?8:32; settings.paraboloidSubdivision=coarse?2:3;
        settings.randomSeed=(reference?0x7f4a7c15u:42u)+static_cast<std::uint32_t>(sample)*0x9e3779b9u;
        settings.selection.randomSeed=settings.randomSeed^0x68bc21ebu;
        RT::DepthPhotonTransport transport; RT::PhotonMap photons;
        if(!transport.build(r.gpu,r.scene,settings,photons)) { result.error=transport.getLastError(); return result; }
        for(const auto& s:transport.getStats().bounces) result.depthMaps+=s.depthMaps;
        RT::PhotonGBuffer receivers;
        if(!r.gpu.render(r.scene,photons,camera,edge,edge,0.12,result.indirect,receivers,RT::PhotonContribution::Indirect,0.1,100)) {
            result.error=r.gpu.getLastError(); return result;
        }
        if(r.directGeneration!=generation) {
            RT::PhotonMappingSettings shading; shading.buildPhotonIndex=false; shading.photonMaxDepth=4;
            shading.directLightSamples=32; shading.render.randomSeed=42;
            RT::PhotonMapper mapper(shading); const std::vector<RT::Photon> empty;
            Phantom::Graphics::Imagef zero(edge,edge);
            for(int y=0;y<edge;++y) for(int x=0;x<edge;++x) zero.setColor(x,y,Phantom::Graphics::ColorRGBAf(0,0,0,1));
            if(!mapper.buildWithPhotons(r.scene,empty) || !mapper.shadeGBuffer(receivers,r.direct,&zero)) {
                result.error="Direct shading failed"; return result;
            }
            r.directGeneration=generation;
        }
        result.direct=r.direct;
        result.seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        return result;
    });
}
void PbvrPreviewPanel::present() { image_.present(display(mean_,direct_)); }
bool PbvrPreviewPanel::saveCheckpoint(const std::string& name) {
    return image_.saveResult((demoDirectory_/(name+".png")).string())&&pfm(demoDirectory_/(name+"_indirect.pfm"),mean_);
}
void PbvrPreviewPanel::update() {
    const auto now=std::chrono::steady_clock::now();
    if(demo_ && lastUpdate_.time_since_epoch().count()) frameSeconds_.push_back(std::chrono::duration<double>(now-lastUpdate_).count());
    lastUpdate_=now;
    if(future_.valid() && future_.wait_for(std::chrono::seconds(0))==std::future_status::ready) {
        Pass pass=future_.get();
        if(pass.generation==generation_ && enabled_) {
            if(!pass.error.empty()) {
                error_=pass.error; enabled_=false;
                if(demo_) { demoResponse_="Error:"+error_; demo_=false; }
                return;
            }
            direct_=std::move(pass.direct); lastPassSeconds_=pass.seconds;
            if(pass.reference) {
                if(referenceSum_.empty()) referenceSum_.assign(edge*edge,V(0));
                for(int y=0;y<edge;++y) for(int x=0;x<edge;++x) {
                    const auto p=pass.indirect.getColor(x,y); referenceSum_[y*edge+x]+=V(p.x,p.y,p.z);
                }
                ++referenceCount_;
            } else {
                if(mean_.empty()) { mean_.assign(edge*edge,V(0)); m2_.assign(edge*edge,V(0)); }
                ++sampleCount_; double variance=0,energy=0;
                for(int y=0;y<edge;++y) for(int x=0;x<edge;++x) {
                    const auto p=pass.indirect.getColor(x,y); const V value(p.x,p.y,p.z);
                    const auto i=y*edge+x; const V delta=value-mean_[i]; mean_[i]+=delta/static_cast<double>(sampleCount_);
                    m2_[i]+=delta*(value-mean_[i]); variance+=m2_[i].x+m2_[i].y+m2_[i].z; energy+=glm::dot(mean_[i],mean_[i]);
                }
                standardError_=sampleCount_>1?std::sqrt(std::max(0.0,variance)/(static_cast<double>(sampleCount_)*(sampleCount_-1)*std::max(energy,1e-30))):-1;
                present();
                if(demo_ && pass.coarse) firstImageSeconds_=std::chrono::duration<double>(std::chrono::steady_clock::now()-startTime_).count();
                if(demo_ && !pass.coarse && sampleCount_==32) refine32Seconds_=std::chrono::duration<double>(std::chrono::steady_clock::now()-startTime_).count();
                if(demo_ && (pass.coarse || sampleCount_==1 || sampleCount_==4 || sampleCount_==16 || sampleCount_==32)) {
                    if(!saveCheckpoint(pass.coarse?"preview":"refine_"+std::to_string(sampleCount_))) {
                        error_="Checkpoint save failed"; enabled_=false; demo_=false; demoResponse_="Error:"+error_; return;
                    }
                }
            }
            if(demo_) metrics_.push_back({generation_,pass.coarse,pass.reference,pass.reference?referenceCount_:sampleCount_,pass.seconds,standardError_,pass.depthMaps});
            if(demo_ && !pass.coarse && !reference_ && sampleCount_>=maxSamples_) reference_=true;
            if(demo_ && reference_ && referenceCount_>=32) finishDemo();
        }
    }
    if(!enabled_ || !error_.empty()) return;
    if(coarse_ && sampleCount_>0 && std::chrono::duration<double>(now-lastChange_).count()>=0.35) {
        ++generation_; coarse_=false; sampleCount_=0; mean_.clear(); m2_.clear(); standardError_=-1;
    }
    if(!future_.valid() && ((coarse_ && sampleCount_==0) || (!coarse_ && (maxSamples_==0 || sampleCount_<maxSamples_)) || (demo_ && reference_))) launch();
}
void PbvrPreviewPanel::finishDemo() {
    for(auto& value:referenceSum_) value/=static_cast<double>(referenceCount_);
    Phantom::Graphics::ImageFileWriter writer;
    bool ok=writer.write((demoDirectory_/"reference.png").string(),display(referenceSum_,direct_))&&pfm(demoDirectory_/"reference_indirect.pfm",referenceSum_);
    std::ofstream metrics(demoDirectory_/"passes.csv"); metrics.precision(12);
    metrics<<"generation,quality,pass,seconds,estimated_relative_standard_error,depth_maps\n";
    for(const auto& m:metrics_) metrics<<m.generation<<','<<(m.coarse?"preview":(m.reference?"reference":"refine"))<<','<<m.pass<<','<<m.seconds<<','<<m.samplingError<<','<<m.maps<<'\n';
    std::ofstream frames(demoDirectory_/"frames.csv"); frames<<"seconds\n"; for(double v:frameSeconds_) frames<<v<<'\n';
    std::ofstream timings(demoDirectory_/"ui_timings.csv"); timings.precision(12);
    timings<<"first_image_seconds,refine_32_seconds,demo_total_seconds\n"<<firstImageSeconds_<<','<<refine32Seconds_<<','
        <<std::chrono::duration<double>(std::chrono::steady_clock::now()-startTime_).count()<<'\n';
    ok=ok&&metrics.good()&&frames.good()&&timings.good(); enabled_=false; demo_=false; reference_=false;
    demoResponse_=ok?"OK:PBVR samples=32 reference=32":"Error:PBVR report write failed";
    if(!ok) error_="PBVR report write failed";
}
void PbvrPreviewPanel::onImGui() {
    if(!shell_ || !shell_->beginPanel("PBVR Progressive Preview")) return;
    ImGui::TextUnformatted("Cornell box / paraboloid photon transport");
    ImGui::BeginDisabled(demo_ || locked_);
    if(ImGui::Button("Start / reset")) start(); ImGui::SameLine();
    if(ImGui::Button(enabled_?"Pause":"Resume")) { enabled_=!enabled_; if(enabled_ && !generation_) start(); }
    ImGui::SameLine(); if(ImGui::Button("Save demo")) startDemo("pbvr_preview_demo");
    bool changed=ImGui::SliderFloat("Yaw",&yaw_,-75,75);
    changed|=ImGui::SliderFloat("Pitch",&pitch_,-25,25);
    changed|=ImGui::SliderFloat("Distance",&distance_,2.8f,7.f);
    ImGui::InputInt("Pass limit (0: continuous)",&maxSamples_); maxSamples_=std::clamp(maxSamples_,0,4096);
    if(changed) start();
    ImGui::EndDisabled();
    ImGui::Text("%s | generation %llu",coarse_?"Preview / settling":(reference_?"Measuring reference":"Refining"),static_cast<unsigned long long>(generation_));
    if(maxSamples_>0) ImGui::Text("Passes %d/%d | last update %.1f ms",samples(),maxSamples_,lastPassSeconds_*1000);
    else ImGui::Text("Passes %d (continuous) | last update %.1f ms",samples(),lastPassSeconds_*1000);
    if(samples()>1) ImGui::Text("Estimated indirect sampling noise: %.1f%%",standardError_*100);
    if(reference_) ImGui::Text("Independent reference: %d/32",referenceCount_);
    ImGui::TextUnformatted("Drag image to orbit. Camera/quality changes reset accumulation.");
    if(!error_.empty()) ImGui::TextWrapped("%s",error_.c_str());
    image_.drawImage();
    if(!demo_ && !locked_ && ImGui::IsItemHovered() && ImGui::IsMouseDragging(0)) {
        const auto delta=ImGui::GetIO().MouseDelta;
        if(delta.x!=0 || delta.y!=0) { yaw_=std::clamp(yaw_+delta.x*0.25f,-75.f,75.f); pitch_=std::clamp(pitch_+delta.y*0.25f,-25.f,25.f); start(); }
    }
    shell_->endPanel();
}
