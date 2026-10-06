#pragma once

#include "../../CGLib/VkAppBase/VkAppBase.h"
#include "../../CGLib/VkAppBase/ScenarioRunner/ScenarioRunner.h"
#include "../../CGLib/VkAppBase/ScenarioRunner/IScenarioHost.h"
#include "../../CGLib/VkAppBase/ScenarioRunner/ScenarioBrowserPanel.h"
#include "../../CGLib/VkAppBase/ScenarioRunner/ViewShell.h"
#include "../../CGLib/GltfRenderer/Gltf/GltfDocument.h"
#include "../../CGLib/GltfRenderer/Renderer/GltfSceneRenderer.h"
#include "../RayTracer/PathTracer.h"
#include "MenuPanel.h"
#include "RayTraceResultPanel.h"
#include "CommandDispatcher.h"

#include <filesystem>
#include <optional>
#include <string>

class RayTracerApp : public ::VKG::VkAppBase, public ::IScenarioHost {
public:
    RayTracerApp(int width, int height, const std::string& title);

    void loadGltf(const std::filesystem::path& path);

    // Phase 4B item 5 "same camera asset": the document's own first Perspective camera node,
    // captured by loadGltf()/reloadFile() (same "first instance" policy as Universe's
    // Renderer::applyAssetCamera()). Orthographic-only/camera-less documents leave this unset.
    bool hasAssetCamera() const { return assetCamera_.has_value(); }
    bool useAssetCamera() const { return useAssetCamera_; }
    void setUseAssetCamera(bool use) { useAssetCamera_ = use && hasAssetCamera(); }

    // Phase 4B item 5 "report the raster/offline diff": saves the most recently completed
    // path-traced render (see RayTraceResultPanel::saveResult()) to a PNG file.
    bool saveRayTraceResult(const std::string& path) const { return resultPanel_.saveResult(path); }

    bool loadScenario(const std::string& jsonPath) override { return runner_.load(jsonPath); }
    void setExitOnScenarioComplete(bool v)         override { exitOnComplete_ = v; }
    int  getExitCode() const { return exitCode_; }

    // IScenarioHost (driven by ScenarioBrowserPanel)
    bool   isScenarioActive()   const override { return runner_.isActive();   }
    bool   scenarioHasFailed()  const override { return runner_.hasFailed();  }
    const std::string& scenarioFailMessage() const override { return runner_.failMessage(); }
    size_t scenarioStepCount()  const override { return runner_.stepCount();  }

protected:
    void onInit()                      override;
    void onUpdate(uint32_t frameIndex) override;
    void onSwapChainCreated()          override;
    void onImGui()                     override;
    void onImGuiReady()                override;
    void onCleanup()                   override;

private:
    Phantom::Gltf::GltfDocument      doc_;
    Phantom::Gltf::GltfSceneRenderer renderer_;
    MenuPanel   menuPanel_;
    RayTraceResultPanel  resultPanel_;

    ViewShell         shell_;   // Command / Outliner windows
    CommandDispatcher dispatcher_;
    ScenarioRunner             runner_;
    ScenarioBrowserPanel       scenarioBrowser_;

    std::optional<std::filesystem::path> pendingPath_;

    bool        screenshotPending_ = false;
    std::string screenshotPendingPath_;
    bool        rayTracePending_ = false;

    std::optional<Phantom::RayTracer::RtCameraSpec> assetCamera_;
    bool useAssetCamera_ = false;

    bool exitOnComplete_ = true;
    int  exitCode_ = 0;

    void setupCallbacks();
    void reloadFile(const std::filesystem::path& path);
    void onRayTrace(int width, int height, int spp, int depth);
    void applyShaders();
};
