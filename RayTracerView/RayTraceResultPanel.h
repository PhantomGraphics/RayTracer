#pragma once

#include "../../CGLib/VkAppBase/IVkSubRenderer.h"
#include "../../CGLib/VulkanGraphics/VulkanSampler.h"
#include "../RayTracer/PathTracer.h"

#include <future>
#include <vector>
#include <vulkan/vulkan.h>

namespace Phantom::VKG {
    class VulkanContext;
    class VulkanCommandPool;
}

class RayTraceResultPanel : public ::VKG::IVkUIPanel {
public:
    void init(const Phantom::VKG::VulkanContext* ctx, const Phantom::VKG::VulkanCommandPool* pool);

    // Render Cornell Box (fallback when no glTF is loaded)
    void triggerRender(const Phantom::RayTracer::RtCameraSpec&   cam,
                       const Phantom::RayTracer::RenderSettings& settings);

    // Render a glTF scene expressed as triangles + textures
    void triggerRenderGltf(const Phantom::RayTracer::RtCameraSpec&              cam,
                           const Phantom::RayTracer::RenderSettings&            settings,
                           std::vector<Phantom::RayTracer::RtTriangle>          triangles,
                           std::vector<Phantom::RayTracer::RtTexture>           textures = {});

    bool isRendering() const { return rendering_; }
    int  lastWidth()   const { return texW_; }
    int  lastHeight()  const { return texH_; }

    void onImGui() override;

    void cleanup(VkDevice device);

private:
    const Phantom::VKG::VulkanContext*     ctx_  = nullptr;
    const Phantom::VKG::VulkanCommandPool* pool_ = nullptr;

    std::future<Phantom::Graphics::Imageuc> future_;
    bool rendering_ = false;

    VkImage        texImage_   = VK_NULL_HANDLE;
    VkDeviceMemory texMemory_  = VK_NULL_HANDLE;
    VkImageView    texView_    = VK_NULL_HANDLE;
    Phantom::VKG::VulkanSampler sampler_;
    VkDescriptorSet    texDescSet_ = VK_NULL_HANDLE;
    int texW_ = 0, texH_ = 0;

    void destroyTexture();
    void uploadTexture(const Phantom::Graphics::Imageuc& img);
};
