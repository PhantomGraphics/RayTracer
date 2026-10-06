#pragma once

#include "../../CGLib/VkAppBase/ScenarioRunner/IScenarioDispatcher.h"
#include "../../CGLib/VkAppBase/ScenarioRunner/UiCommand.h"
#include "../../CGLib/VkAppBase/ScenarioRunner/CommandQueue.h"
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
    std::vector<CommandInfo> commandCatalog() const override;

    // GUI operations take the same queue and handlers as typed commands; the
    // response is discarded (see UiCommand.h).
    void submitUi(const std::string& cmd) { dispatch(markUiCommand(cmd)); }

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
    std::string cmdCheckCommandCatalog();

    const Phantom::Gltf::GltfDocument* doc_ = nullptr;
    Phantom::Gltf::GltfSceneRenderer* renderer_ = nullptr;
    RayTracerApp* app_ = nullptr;

    std::optional<std::filesystem::path> pendingLoad_;
    std::optional<std::filesystem::path> pendingScreenshot_;
    std::optional<RayTraceRequest>       pendingRayTrace_;

    // Deferred answers (LoadFile / SaveScreenshot / RunRayTrace) of GUI-originated
    // commands: nobody reads them, so the matching signal*() drops one each.
    int silentDeferred_ = 0;
    void pushDeferred(std::string resp);

    CommandQueue queue_;
    std::mutex   deferredMutex_;   // guards silentDeferred_ (set on the render thread, read by signal*())
};
