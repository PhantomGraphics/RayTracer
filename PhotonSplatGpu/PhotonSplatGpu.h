#pragma once
#include "../RayTracer/PhotonMapper.h"
#include <memory>
#include <string>

namespace Phantom::VKG { class VulkanContext; class VulkanCommandPool; }
namespace Phantom::RayTracer {
struct PhotonSplatStats {
    double uploadSeconds = 0;
    double submitReadbackSeconds = 0;
    double gbufferGpuMilliseconds = -1;
    double splatGpuMilliseconds = -1;
    double copyGpuMilliseconds = -1;
    std::size_t splattedPhotons = 0;
};
// Offscreen Vulkan rasterization + additive photon splatting. No window needed.
// Context/pool must outlive this object. All triangles must be diffuse.
class PhotonSplatGpu {
public:
    PhotonSplatGpu();
    ~PhotonSplatGpu();
    PhotonSplatGpu(const PhotonSplatGpu&) = delete;
    PhotonSplatGpu& operator=(const PhotonSplatGpu&) = delete;
    bool create(VKG::VulkanContext& context, const VKG::VulkanCommandPool& pool,
                const std::string& shaderDirectory);
    void destroy();
    // Depth-tested rasterization only: no photon splatting or transport rays.
    bool rasterize(const std::vector<RtTriangle>& triangles, const RtCameraSpec& camera,
        int width, int height, PhotonGBuffer& receivers,
        double nearPlane = 0.01, double farPlane = 10000.0);
    // Photon positions are quantized to float on upload. Depth writes occur
    // only in the geometry pass; photons add all matching contributions.
    bool render(const std::vector<RtTriangle>& triangles, const PhotonMap& photons,
        const RtCameraSpec& camera, int width, int height, double radius,
        Graphics::Imagef& indirect, PhotonGBuffer& receivers,
        PhotonContribution contribution = PhotonContribution::Indirect,
        double nearPlane = 0.01, double farPlane = 10000.0);
    const std::string& getLastError() const;
    const PhotonSplatStats& getStats() const;
private:
    bool renderImpl(const std::vector<RtTriangle>& triangles, const PhotonMap& photons,
        const RtCameraSpec& camera, int width, int height, double radius,
        Graphics::Imagef& indirect, PhotonGBuffer& receivers,
        PhotonContribution contribution, double nearPlane, double farPlane, bool geometryOnly);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace Phantom::RayTracer
