#pragma once

#include "../../CGLib/VkAppBase/VkAppBase.h"
#include "../../CGLib/VkAppBase/ScenarioRunner/ScenarioRunner.h"
#include "../../CGLib/VkAppBase/ScenarioRunner/IScenarioHost.h"
#include "../../CGLib/VkAppBase/ScenarioRunner/ScenarioBrowserPanel.h"
#include "../../CGLib/GltfRenderer/Gltf/GltfDocument.h"
#include "../../CGLib/GltfRenderer/Renderer/GltfSceneRenderer.h"
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
    void onCleanup()                   override;

private:
    Phantom::Gltf::GltfDocument      doc_;
    Phantom::Gltf::GltfSceneRenderer renderer_;
    MenuPanel   menuPanel_;
    RayTraceResultPanel  resultPanel_;

    CommandDispatcher dispatcher_;
    ScenarioRunner             runner_;
    ScenarioBrowserPanel       scenarioBrowser_;

    std::optional<std::filesystem::path> pendingPath_;

    bool        screenshotPending_ = false;
    std::string screenshotPendingPath_;
    bool        rayTracePending_ = false;

    bool exitOnComplete_ = true;
    int  exitCode_ = 0;

    void setupCallbacks();
    void reloadFile(const std::filesystem::path& path);
    void onRayTrace(int width, int height, int spp, int depth);
    void applyShaders();
};
