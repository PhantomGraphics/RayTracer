#pragma once
#include "PhotonSplatGpu.h"

namespace Phantom::RayTracer {
struct DepthPhotonSettings {
    int lightSamples = 8;
    int maxDepth = 4;
    int firstResolution = 32;
    int minResolution = 8;
    int resolutionDropEvery = 1; // Halve edge length each N bounces; 0 disables.
    double nearPlane = 0.001;
    double farPlane = 100;
    double sourceOffset = 0.0001;
    std::size_t maxStoredPhotons = 2000000; // Fail rather than truncate light.
    bool storeDirectPhotons = false;
    bool buildPhotonIndex = false;
    std::uint32_t randomSeed = 42;
    PbvrPhotonSettings selection = [] {
        PbvrPhotonSettings s; s.selection=PbvrSelection::Power;
        s.retention=0.5; s.maxParticles=64; return s;
    }();
};
struct DepthPhotonBounceStats {
    int resolution = 0;
    std::size_t sources = 0, depthMaps = 0, arrivals = 0, retained = 0;
    Math::Vector3dd launchedFlux{0.0}, arrivedFlux{0.0}, escapedFlux{0.0};
};
struct DepthPhotonStats {
    double seconds = 0;
    std::vector<DepthPhotonBounceStats> bounces;
};
// Diffuse, untextured hemicube transport. GPU depth tests find ALL arrivals;
// CPU readback performs flux accounting and source selection, never ray hits.
// Rasterizer must be created and remain alive. Output/stats clear on failure.
class DepthPhotonTransport {
public:
    bool build(PhotonSplatGpu& rasterizer,const std::vector<RtTriangle>& triangles,
        const DepthPhotonSettings& settings,PhotonMap& output);
    const std::string& getLastError() const { return error_; }
    const DepthPhotonStats& getStats() const { return stats_; }
private:
    std::string error_;
    DepthPhotonStats stats_;
};
} // namespace Phantom::RayTracer
