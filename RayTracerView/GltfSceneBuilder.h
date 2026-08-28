#pragma once

#include "../../RayTracer/RayTracer/PathTracer.h"
#include "../../CGLib/GltfRenderer/Gltf/GltfDocument.h"

#include <vector>

// Result of converting a GltfDocument for CPU path tracing.
// triangles: world-space triangles with UV and texture indices set.
// textures:  one RtTexture per GltfTexture entry (indexed the same way).
struct GltfBuildResult {
    std::vector<Phantom::RayTracer::RtTriangle> triangles;
    std::vector<Phantom::RayTracer::RtTexture>  textures;
};

// Full conversion: triangles + textures.
GltfBuildResult buildFromGltf(const Phantom::Gltf::GltfDocument& doc);

// Legacy: triangles only (textures discarded). All triangles are in world space.
std::vector<Phantom::RayTracer::RtTriangle> buildTrianglesFromGltf(const Phantom::Gltf::GltfDocument& doc);
