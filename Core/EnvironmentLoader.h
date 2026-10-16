#pragma once

#include "PathTracer.h"

#include <memory>
#include <string>

namespace Phantom::RayTracer {

// Decodes an equirectangular .hdr panorama (UTF-8 path) for the CPU path tracer. Panoramas wider than 1024 texels are
// box-filtered down (a sky does not need more for diffuse bounces, and the tracer reads it per ray). The decoded image is
// cached (one entry) by path and modification time, so changing only the intensity or background is cheap; `intensity` and
// `background` are applied to a copy on every call.
// Returns null for an empty path, a missing or unreadable file, or an empty image (the tracer then keeps its gradient sky).
std::shared_ptr<const RtEnvironment> loadEnvironment(const std::string& path, float intensity, bool background);

} // namespace Phantom::RayTracer
