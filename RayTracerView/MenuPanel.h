#pragma once

#include "../../CGLib/VkAppBase/IVkSubRenderer.h"
#include "../../CGLib/GltfRenderer/Renderer/GltfSceneRenderer.h"
#include "../../CGLib/UIWidgets/FileOpenView.h"

#include <filesystem>
#include <functional>

class MenuPanel : public ::VKG::IVkUIPanel {
public:
    void init(Phantom::Gltf::GltfSceneRenderer* renderer,
              std::function<void(const std::filesystem::path&)> onFileOpen,
              std::function<void(int, int, int, int)>           onRayTrace);

    void setFilePath(const std::filesystem::path& p) { filePath_ = p; }
    bool isRendering() const { return rendering_; }
    void setRendering(bool v) { rendering_ = v; }

    void onImGui() override;

private:
    Phantom::Gltf::GltfSceneRenderer* renderer_ = nullptr;
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
