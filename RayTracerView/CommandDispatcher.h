#pragma once

#include "../../CGLib/VkAppBase/ScenarioRunner/IScenarioDispatcher.h"
#include "../../CGLib/GltfRenderer/Gltf/GltfDocument.h"
#include "../../CGLib/GltfRenderer/Renderer/GltfSceneRenderer.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <vector>

class RayTracerApp; // fwd decl -- avoids a circular include with RayTracerApp.h

class CommandDispatcher : public IScenarioDispatcher {
public:
    void setDocument(const Phantom::Gltf::GltfDocument* doc) { doc_ = doc; }
    void setRenderer(Phantom::Gltf::GltfSceneRenderer* r) { renderer_ = r; }
    // Non-owning; needed for the asset-camera toggle and the raster/offline report commands
    // (Phase 4B item 5), which live on RayTracerApp rather than doc_/renderer_.
    void setApp(RayTracerApp* app) { app_ = app; }

    void processQueue();

    void dispatch(const std::string& command) override;
    std::vector<std::string> collectResponses() override;

    std::optional<std::filesystem::path> takePendingLoad();
    void signalLoaded(bool ok, const std::string& msg = {});

    std::optional<std::filesystem::path> takePendingScreenshot();
    void signalScreenshotDone(bool ok, const std::string& path);

    struct RayTraceRequest {
        int width = 0;
        int height = 0;
        int spp = 0;
        int depth = 0;
    };
    std::optional<RayTraceRequest> takePendingRayTrace();
    void signalRayTraceDone(bool ok, int width, int height);

private:
    std::string route(const std::string& cmd);

    const Phantom::Gltf::GltfDocument* doc_ = nullptr;
    Phantom::Gltf::GltfSceneRenderer* renderer_ = nullptr;
    RayTracerApp* app_ = nullptr;

    std::optional<std::filesystem::path> pendingLoad_;
    std::optional<std::filesystem::path> pendingScreenshot_;
    std::optional<RayTraceRequest>       pendingRayTrace_;

    std::mutex              mutex_;
    std::queue<std::string> inputQueue_;
    std::queue<std::string> outputQueue_;
};
