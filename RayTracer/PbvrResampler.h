#pragma once
#include "PhotonMapper.h"

namespace Phantom::RayTracer {
struct PbvrArrival {
    Math::Vector3dd position{0.0};
    Math::Vector3dd power{0.0}; // Already multiplied by material reflectance.
    int surface = -1;
};
struct PbvrSelectedSample {
    std::size_t source = 0;
    double powerScale = 1;
    std::uint64_t randomKey = 0;
    bool resampled = false;
};
struct PbvrResampleStats { std::size_t strata = 0, sampledStrata = 0; };

// Power / SpatialPower selection. Keeps ceil(N*retention), bounded by maxParticles.
// All power must be finite, nonnegative and nonzero. spatialCellSize must be
// explicit and positive for SpatialPower (PhotonMapper resolves its default).
// Uses stratified inverse-CDF draws; repeated sources get independent ray keys.
// Returns probability-corrected scales, never averaged/relocated positions.
// Failure clears output and stats and reports an error.
bool resamplePbvr(const std::vector<PbvrArrival>& arrivals,const PbvrPhotonSettings& settings,
    std::uint32_t seed,std::vector<PbvrSelectedSample>& selected,
    PbvrResampleStats& stats,std::string& error);
} // namespace Phantom::RayTracer
