#include "pch.h"
#include "MenuPanel.h"
#include "imgui.h"

using namespace Phantom::Gltf;

void MenuPanel::init(
    GltfSceneRenderer* renderer,
    std::function<void(const std::filesystem::path&)> onFileOpen,
    std::function<void(int, int, int, int)>           onRayTrace)
{
    renderer_   = renderer;
    onFileOpen_ = std::move(onFileOpen);
    onRayTrace_ = std::move(onRayTrace);

    fileOpenView_.addFilter("*.gltf");
    fileOpenView_.addFilter("*.glb");
}

void MenuPanel::onImGui()
{
    ImGui::Begin("RayTracer View");

    // --- File section ---
    fileOpenView_.show();
    if (ImGui::Button("Load")) {
        const std::string path = fileOpenView_.getFileName();
        if (!path.empty() && onFileOpen_)
            onFileOpen_(std::filesystem::path(path));
    }

    ImGui::Separator();

    if (filePath_.empty()) {
        ImGui::Text("File: <none>");
    } else {
        ImGui::Text("File: %s", filePath_.filename().string().c_str());
    }

    // --- Camera info ---
    if (renderer_) {
        ImGui::Separator();
        ImGui::SliderFloat("Camera Distance", renderer_->camDistPtr(), 0.1f, 100.f);
        ImGui::SliderFloat3("Camera Target",  &renderer_->camTargetPtr()->x, -10.f, 10.f);

        const GltfDocument* doc = renderer_->document();
        if (doc && !doc->meshes.empty()) {
            ImGui::Separator();
            ImGui::Text("Meshes   : %d", (int)doc->meshes.size());
            ImGui::Text("Materials: %d", (int)doc->materials.size());
            ImGui::Text("Textures : %d", (int)doc->textures.size());
        }
    }

    // --- Ray trace settings ---
    ImGui::Separator();
    ImGui::Text("Ray Trace Settings");
    ImGui::Checkbox("Lock aspect to viewport", &lockAspectToViewport_);
    ImGui::InputInt("Width##rt",  &rtWidth_);
    if (lockAspectToViewport_) ImGui::BeginDisabled();
    ImGui::InputInt("Height##rt", &rtHeight_);
    if (lockAspectToViewport_) ImGui::EndDisabled();
    ImGui::InputInt("SPP##rt",    &rtSpp_);
    ImGui::InputInt("Depth##rt",  &rtDepth_);

    rtWidth_  = std::max(1, std::min(rtWidth_,  4096));
    rtHeight_ = std::max(1, std::min(rtHeight_, 4096));
    rtSpp_    = std::max(1, std::min(rtSpp_,    4096));
    rtDepth_  = std::max(1, std::min(rtDepth_,   64));

    if (lockAspectToViewport_ && renderer_) {
        const VkExtent2D ext = renderer_->getExtent();
        if (ext.width > 0 && ext.height > 0) {
            const float aspect = static_cast<float>(ext.width) / static_cast<float>(ext.height);
            rtHeight_ = std::max(1, static_cast<int>(std::lround(rtWidth_ / aspect)));
        }
    }

    ImGui::Spacing();

    if (rendering_) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Ray Trace") && onRayTrace_) {
        onRayTrace_(rtWidth_, rtHeight_, rtSpp_, rtDepth_);
    }
    if (rendering_) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("Rendering...");
    }

    ImGui::End();
}
