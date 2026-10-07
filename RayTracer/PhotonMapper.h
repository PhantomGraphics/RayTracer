#pragma once

#include "PathTracer.h"
#include <memory>

namespace Phantom::RayTracer {

enum class PhotonContribution { All, Direct, Indirect, Caustic };
enum class PhotonTransport { Path, Pbvr }; // Per-path roulette / staged particle transport.
enum class PbvrSelection { Uniform, Power, SpatialPower };

struct PbvrPhotonSettings {
    double retention = 0.5; // Independent survival on each re-emission; (0, 1].
    std::size_t maxParticles = 0; // Cap after thinning; 0 means unlimited.
    std::uint32_t randomSeed = 42; // Thinning only; emission uses render.randomSeed.
    PbvrSelection selection = PbvrSelection::Uniform;
    double uniformMix = 0.05; // Uniform probability floor in importance sampling.
    double spatialCellSize = 0.0; // SpatialPower: 0 uses gatherRadius.
};

struct Photon {
    Math::Vector3dd position{0.0};
    Math::Vector3dd direction{0.0, -1.0, 0.0}; // Incoming propagation direction.
    Math::Vector3dd normal{0.0, 1.0, 0.0};
    Math::Vector3dd power{0.0}; // Flux, emission normalization + resampling compensation already applied.
    PhotonContribution contribution = PhotonContribution::Direct;
    int bounce = 0; // Arrival order: 0 is the first surface reached from a light.
};

class PhotonMap {
public:
    PhotonMap();
    ~PhotonMap();
    PhotonMap(const PhotonMap&) = delete;
    PhotonMap& operator=(const PhotonMap&) = delete;
    // Replaces the map. Invalid input clears it and returns false.
    bool build(const std::vector<Photon>& photons, bool buildIndex = true);
    bool build(std::vector<Photon>&& photons, bool buildIndex = true);
    void clear();
    const std::vector<Photon>& getPhotons() const;
    bool hasSpatialIndex() const;
    // Fixed-radius, uniform surface kernel. Indirect includes caustics.
    // Normal / plane filtering reduces nearby-surface leaks, but is approximate.
    // Without an index this uses an exact linear scan (slower, never black silently).
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
    PhotonTransport transport = PhotonTransport::Path;
    PbvrPhotonSettings pbvr;
    bool buildPhotonIndex = true; // Disable when using GPU gathering.
    bool storeDirectPhotons = true; // Optional: direct light is evaluated by NEE.
};

struct PhotonBounceStats {
    std::size_t tracedRays = 0;
    std::size_t arrivals = 0; // Front-facing arrivals, including mirrors.
    std::size_t storedPhotons = 0; // Diffuse arrivals saved before thinning.
    std::size_t continuationCandidates = 0; // Nonzero reflected power.
    std::size_t retainedParticles = 0; // Re-emitted after thinning + cap.
    Math::Vector3dd storedFlux{0.0};
    Math::Vector3dd reemittedFlux{0.0}; // Includes reflectance and selection compensation.
    std::size_t spatialStrata = 0;
    std::size_t sampledStrata = 0;
};
struct PhotonMappingStats {
    std::size_t emittedPhotons = 0;
    std::size_t directPhotons = 0;
    std::size_t indirectPhotons = 0; // Excludes causticPhotons.
    std::size_t causticPhotons = 0;
    double buildSeconds = 0.0;
    double transportSeconds = 0.0; // Excludes BVH and KDTree construction.
    double mapBuildSeconds = 0.0;
    std::size_t tracedRays = 0;
    std::vector<PhotonBounceStats> bounces;
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
    // Initialize final shading with externally transported photons. Does not
    // emit/trace any transport rays; BVH is retained for camera/direct shading.
    bool buildWithPhotons(const std::vector<RtTriangle>& triangles,
        const std::vector<Photon>& photons);
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
    bool buildImpl(const std::vector<RtTriangle>& triangles,const std::vector<Photon>* photons);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace Phantom::RayTracer
