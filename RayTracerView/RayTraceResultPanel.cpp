#include "pch.h"
#include "RayTraceResultPanel.h"

#include "../../CGLib/VulkanGraphics/VulkanContext.h"
#include "../../CGLib/VulkanGraphics/VulkanCommandPool.h"
#include "../../CGLib/VulkanGraphics/VulkanImage.h"

#include "backends/imgui_impl_vulkan.h"
#include "imgui.h"

#include <cassert>
#include <cstring>
#include <stdexcept>

// ============================================================
//  Helper: upload RGBA8 pixels to a Vulkan sampled image
// ============================================================

static void createTextureFromPixels(
    const Phantom::VKG::VulkanContext&     ctx,
    const Phantom::VKG::VulkanCommandPool& pool,
    const uint8_t* pixels, int w, int h,
    VkImage& outImage, VkDeviceMemory& outMemory, VkImageView& outView)
{
    VkDeviceSize imageSize = static_cast<VkDeviceSize>(w) * h * 4;

    VkBuffer       stagingBuf;
    VkDeviceMemory stagingMem;
    {
        VkBufferCreateInfo bi{};
        bi.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size        = imageSize;
        bi.usage       = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(ctx.getDevice(), &bi, nullptr, &stagingBuf);

        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(ctx.getDevice(), stagingBuf, &mr);
        auto memType = ctx.findMemoryType(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        assert(memType.has_value());

        VkMemoryAllocateInfo ai{};
        ai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize  = mr.size;
        ai.memoryTypeIndex = memType.value_or(0);
        vkAllocateMemory(ctx.getDevice(), &ai, nullptr, &stagingMem);
        vkBindBufferMemory(ctx.getDevice(), stagingBuf, stagingMem, 0);

        void* mapped;
        vkMapMemory(ctx.getDevice(), stagingMem, 0, imageSize, 0, &mapped);
        std::memcpy(mapped, pixels, static_cast<size_t>(imageSize));
        vkUnmapMemory(ctx.getDevice(), stagingMem);
    }

    Phantom::VKG::VulkanImage::create(ctx,
        static_cast<uint32_t>(w), static_cast<uint32_t>(h),
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        outImage, outMemory);

    auto cmd = pool.beginSingleTimeCommands();
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image               = outImage;
        barrier.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent      = {(uint32_t)w, (uint32_t)h, 1};
        vkCmdCopyBufferToImage(cmd, stagingBuf, outImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    pool.endSingleTimeCommands(cmd);

    vkDestroyBuffer(ctx.getDevice(), stagingBuf, nullptr);
    vkFreeMemory(ctx.getDevice(), stagingMem, nullptr);

    outView = Phantom::VKG::VulkanImage::createView(ctx.getDevice(), outImage,
                                           VK_FORMAT_R8G8B8A8_UNORM,
                                           VK_IMAGE_ASPECT_COLOR_BIT);
}

// ============================================================
//  RayTraceResultPanel
// ============================================================

void RayTraceResultPanel::init(const Phantom::VKG::VulkanContext*     ctx,
                               const Phantom::VKG::VulkanCommandPool* pool)
{
    ctx_  = ctx;
    pool_ = pool;
}

void RayTraceResultPanel::triggerRender(
    const Phantom::RayTracer::RtCameraSpec&   cam,
    const Phantom::RayTracer::RenderSettings& settings)
{
    if (rendering_) return;
    rendering_ = true;

    future_ = std::async(std::launch::async,
        [cam, settings]() -> Phantom::Graphics::Imageuc {
            Phantom::Graphics::Imageuc img;
            Phantom::RayTracer::PathTracer tracer(settings);
            tracer.render(cam, img);
            return img;
        });
}

void RayTraceResultPanel::triggerRenderGltf(
    const Phantom::RayTracer::RtCameraSpec&         cam,
    const Phantom::RayTracer::RenderSettings&       settings,
    std::vector<Phantom::RayTracer::RtTriangle>     triangles,
    std::vector<Phantom::RayTracer::RtTexture>      textures)
{
    if (rendering_) return;
    rendering_ = true;

    future_ = std::async(std::launch::async,
        [cam, settings,
         tris = std::move(triangles),
         texs = std::move(textures)]() -> Phantom::Graphics::Imageuc
        {
            Phantom::Graphics::Imageuc img;
            Phantom::RayTracer::PathTracer tracer(settings);
            tracer.render(tris, cam, img, texs);
            return img;
        });
}

void RayTraceResultPanel::destroyTexture()
{
    if (!ctx_) return;
    VkDevice dev = ctx_->getDevice();

    if (texDescSet_ != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(texDescSet_);
        texDescSet_ = VK_NULL_HANDLE;
    }
    if (sampler_.isValid())  sampler_.destroy(dev);
    if (texView_)   { vkDestroyImageView(dev, texView_, nullptr);   texView_   = VK_NULL_HANDLE; }
    if (texImage_)  { vkDestroyImage(dev, texImage_, nullptr);       texImage_  = VK_NULL_HANDLE; }
    if (texMemory_) { vkFreeMemory(dev, texMemory_, nullptr);        texMemory_ = VK_NULL_HANDLE; }
    texW_ = texH_ = 0;
}

void RayTraceResultPanel::uploadTexture(const Phantom::Graphics::Imageuc& img)
{
    if (!ctx_ || !pool_) return;

    const int w = img.getWidth();
    const int h = img.getHeight();
    if (w <= 0 || h <= 0) return;

    const auto& pixels = img.getValues();  // std::vector<unsigned char>, RGBA layout

    destroyTexture();

    createTextureFromPixels(*ctx_, *pool_,
        pixels.data(), w, h,
        texImage_, texMemory_, texView_);

    sampler_.create(ctx_->getDevice(), VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    texDescSet_ = ImGui_ImplVulkan_AddTexture(
        sampler_.get(), texView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    texW_ = w;
    texH_ = h;
}

void RayTraceResultPanel::onImGui()
{
    // Poll for completed render
    if (rendering_ && future_.valid()) {
        if (future_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            Phantom::Graphics::Imageuc img = future_.get();
            vkDeviceWaitIdle(ctx_->getDevice());
            uploadTexture(img);
            rendering_ = false;
        }
    }

    ImGui::Begin("Ray Trace Result");

    if (rendering_) {
        ImGui::Text("Rendering... please wait.");
    } else if (texDescSet_ != VK_NULL_HANDLE) {
        ImGui::Text("Size: %d x %d", texW_, texH_);
        const float avail = ImGui::GetContentRegionAvail().x;
        const float aspect = (texH_ > 0) ? static_cast<float>(texW_) / texH_ : 1.f;
        const float dispH = avail / aspect;
        ImGui::Image((ImTextureID)(intptr_t)texDescSet_, ImVec2(avail, dispH));
    } else {
        ImGui::Text("No result yet. Press \"Ray Trace\" to render.");
    }

    ImGui::End();
}

void RayTraceResultPanel::cleanup(VkDevice)
{
    destroyTexture();
}
