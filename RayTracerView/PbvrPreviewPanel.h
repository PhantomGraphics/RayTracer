#pragma once
#include "RayTraceResultPanel.h"
#include "../../CGLib/Math/Vector3d.h"
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>

// Built-in Cornell experiment. Worker owns a separate Vulkan device/queue;
// the viewer never shares its command pool with the offscreen transport.
class PbvrPreviewPanel {
public:
    PbvrPreviewPanel();
    ~PbvrPreviewPanel();
    void init(Phantom::VKG::VulkanContext& context,const Phantom::VKG::VulkanCommandPool& pool,ViewShell& shell);
    void update();
    void onImGui();
    void setLocked(bool locked) { locked_=locked; }
    void cleanup(VkDevice device);
    void start();
    bool startDemo(const std::filesystem::path& directory);
    bool setYaw(float degrees);
    int samples() const { return coarse_?0:sampleCount_; }
    std::uint64_t generation() const { return generation_; }
    const std::string& error() const { return error_; }
    std::optional<std::string> takeDemoResponse();
private:
    using V=Phantom::Math::Vector3dd;
    struct Runtime;
    struct Pass {
        std::uint64_t generation=0;
        bool coarse=false,reference=false;
        Phantom::Graphics::Imagef indirect,direct;
        double seconds=0;
        std::size_t depthMaps=0;
        std::string error;
    };
    std::unique_ptr<Runtime> runtime_;
    std::future<Pass> future_;
    RayTraceResultPanel image_;
    ViewShell* shell_=nullptr;
    bool enabled_=false,coarse_=true,demo_=false,reference_=false;
    bool locked_=false;
    float yaw_=0,pitch_=0,distance_=4.2f;
    int sampleCount_=0,maxSamples_=0,referenceCount_=0; // 0 = continuous.
    std::uint64_t generation_=0;
    std::vector<V> mean_,m2_,referenceSum_;
    Phantom::Graphics::Imagef direct_;
    std::string error_,shaderDirectory_;
    std::filesystem::path demoDirectory_;
    std::optional<std::string> demoResponse_;
    std::chrono::steady_clock::time_point lastChange_,lastUpdate_,startTime_;
    double lastPassSeconds_=0,standardError_=-1; // -1: variance unavailable.
    double firstImageSeconds_=-1,refine32Seconds_=-1;
    struct Metric { std::uint64_t generation; bool coarse,reference; int pass; double seconds,samplingError; std::size_t maps; };
    std::vector<Metric> metrics_;
    std::vector<double> frameSeconds_;
    void reset();
    void launch();
    void present();
    void finishDemo();
    bool saveCheckpoint(const std::string& name);
};
