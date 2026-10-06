#pragma once

#include "../../CGLib/VkAppBase/IVkSubRenderer.h"
#include "../../CGLib/GltfRenderer/Renderer/GltfSceneRenderer.h"
#include "../../CGLib/UIWidgets/FileOpenView.h"

#include <filesystem>
#include <functional>

class RayTracerApp; // fwd decl -- avoids a circular include with RayTracerApp.h
class ViewShell;

class MenuPanel : public ::VKG::IVkUIPanel {
public:
    void init(Phantom::Gltf::GltfSceneRenderer* renderer,
              std::function<void(const std::filesystem::path&)> onFileOpen,
              std::function<void(int, int, int, int)>           onRayTrace);

    // Non-owning; needed for the "Use Asset Camera" checkbox (Phase 4B item 5).
    void setApp(RayTracerApp* app) { app_ = app; }

    // The window is a shell panel (hidden until opened); GUI actions are sent as
    // commands through `submit`, i.e. the path a typed or scenario command takes.
    void setShell(ViewShell* s) { shell_ = s; }
    void setSubmit(std::function<void(const std::string&)> f) { submit_ = std::move(f); }
    void setLocked(bool v) { locked_ = v; }   // scenario running: controls shown but disabled

    void setFilePath(const std::filesystem::path& p) { filePath_ = p; }
    bool isRendering() const { return rendering_; }
    void setRendering(bool v) { rendering_ = v; }

    void onImGui() override;

private:
    Phantom::Gltf::GltfSceneRenderer* renderer_ = nullptr;
    RayTracerApp* app_ = nullptr;
    ViewShell* shell_ = nullptr;
    std::function<void(const std::string&)> submit_;
    bool locked_ = false;
    void send(const std::string& cmd) { if (submit_) submit_(cmd); }
    std::function<void(const std::filesystem::path&)> onFileOpen_;
    std::function<void(int, int, int, int)>           onRayTrace_;

    std::filesystem::path filePath_;
    Phantom::UI::FileOpenView fileOpenView_{"Open glTF"};

    int  rtWidth_  = 640;
    int  rtHeight_ = 480;
    int  rtSpp_    = 16;
    int  rtDepth_  = 8;
    bool rendering_ = false;
    bool lockAspectToViewport_ = true;
};
