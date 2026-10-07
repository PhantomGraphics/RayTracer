#pragma once

#include "PathTracer.h"
#include <memory>

namespace Phantom::RayTracer {

enum class PhotonContribution { All, Direct, Indirect, Caustic };

struct Photon {
    Math::Vector3dd position{0.0};
    Math::Vector3dd direction{0.0, -1.0, 0.0}; // Incoming propagation direction.
    Math::Vector3dd normal{0.0, 1.0, 0.0};
    Math::Vector3dd power{0.0}; // Flux, already divided by emitted photon count.
    PhotonContribution contribution = PhotonContribution::Direct;
};

class PhotonMap {
public:
    PhotonMap();
    ~PhotonMap();
    PhotonMap(const PhotonMap&) = delete;
    PhotonMap& operator=(const PhotonMap&) = delete;
    // Replaces the map. Invalid input clears it and returns false.
    bool build(const std::vector<Photon>& photons);
    void clear();
    const std::vector<Photon>& getPhotons() const;
    // Fixed-radius, uniform surface kernel. Indirect includes caustics.
    // Normal / plane filtering reduces nearby-surface leaks, but is approximate.
    Math::Vector3dd estimateRadiance(const Math::Vector3dd& position,
        const Math::Vector3dd& normal, const Math::Vector3dd& reflectance,
        double radius, PhotonContribution contribution = PhotonContribution::All) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct PhotonMappingSettings {
    RenderSettings render;
    std::size_t photonCount = 100000;
    int photonMaxDepth = 8;
    double gatherRadius = 20.0; // World units; default camera uses the 555-unit Cornell box.
    int directLightSamples = 4;
    double rayEpsilon = 1.0e-5;
};

struct PhotonMappingStats {
    std::size_t emittedPhotons = 0;
    std::size_t directPhotons = 0;
    std::size_t indirectPhotons = 0; // Excludes causticPhotons.
    std::size_t causticPhotons = 0;
    double buildSeconds = 0.0;
};

struct PhotonReceiver {
    Math::Vector3dd position{0.0};
    Math::Vector3dd normal{0.0, 1.0, 0.0};
    int triangle = -1;
    float depth = 1.0f; // Vulkan device depth, retained for diagnostics.
};
struct PhotonGBuffer {
    int width = 0, height = 0;
    std::vector<PhotonReceiver> receivers; // Pixel centers, top row first.
};

// Two-phase CPU photon mapper. Supports one-sided emissive triangles,
// Lambertian surfaces (metallic == 0), and ideal mirrors (metallic == 1,
// roughness == 0). Textures, rough metals, transmission and volumes are unsupported.
class PhotonMapper {
public:
    explicit PhotonMapper(const PhotonMappingSettings& settings = {});
    ~PhotonMapper();
    PhotonMapper(const PhotonMapper&) = delete;
    PhotonMapper& operator=(const PhotonMapper&) = delete;
    // Replaces scene + photon map. On failure the mapper becomes uninitialized.
    bool build(const std::vector<RtTriangle>& triangles);
    bool renderLinear(const RtCameraSpec& camera, Graphics::Imagef& output) const;
    bool render(const RtCameraSpec& camera, Graphics::Imageuc& output) const;
    // Diffuse G-buffer shading. Optional linear indirect image replaces KDTree
    // gathering; emission and direct-light random streams remain identical.
    bool shadeGBuffer(const PhotonGBuffer& gbuffer, Graphics::Imagef& output,
        const Graphics::Imagef* indirect = nullptr) const;
    const PhotonMap& getPhotonMap() const;
    const PhotonMappingStats& getStats() const;
    const std::string& getLastError() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace Phantom::RayTracer
