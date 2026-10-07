#pragma once
#include "GltfSceneBuilder.h"
#include "../../CGLib/GltfRenderer/Renderer/GltfSceneRenderer.h"
#include <future>
#include <memory>
#include <chrono>

// Screen-space linear lighting for the native glTF viewport. Binding 8 is the
// renderer's existing application scalar field; only our fragment shader reads it.
class GltfPbvrPreview {
public:
    GltfPbvrPreview();
    ~GltfPbvrPreview();
    void setScene(const Phantom::Gltf::GltfDocument& document);
    void update(Phantom::Gltf::GltfSceneRenderer& renderer);
    void drawControls();
    void cleanup();
    void setEnabled(bool value) { if(enabled_!=value) { enabled_=value; invalidate(); } }
    void setIndirectGain(float value) { indirectGain_=value; dirty_=true; }
    int samples() const { return coarse_?0:samples_; }
    std::uint64_t generation() const { return generation_; }
    bool available() const { return enabled_ && static_cast<bool>(scene_); }
    const std::string& status() const { return status_; }
private:
    struct Runtime;
    struct Result {
        std::uint64_t generation=0;
        bool coarse=false;
        double seconds=0;
        std::string error;
        std::vector<glm::vec4> lighting,direct,positions,normals;
    };
    std::unique_ptr<Runtime> runtime_;
    std::future<Result> future_;
    std::shared_ptr<const GltfBuildResult> scene_;
    std::vector<glm::vec4> mean_,direct_,positions_,normals_;
    float indirectGain_=1;
    Phantom::Gltf::RtCameraParams camera_{};
    VkExtent2D extent_{};
    std::uint64_t generation_=0;
    int samples_=0,width_=96,height_=96;
    bool enabled_=true,coarse_=true,dirty_=true,haveCamera_=false;
    double radius_=0.12,lastSeconds_=0;
    std::string status_="Load a diffuse emissive glTF scene";
    std::chrono::steady_clock::time_point changed_;
    void invalidate();
    void launch();
};
