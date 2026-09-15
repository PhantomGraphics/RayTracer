#pragma once

#include "../../RayTracer/RayTracer/PathTracer.h"
#include "../../CGLib/GltfRenderer/Gltf/GltfDocument.h"

#include <optional>
#include <vector>

// Result of converting a GltfDocument for CPU path tracing.
// triangles: world-space triangles with UV and texture indices set.
// textures:  one RtTexture per GltfTexture entry (indexed the same way).
// lights:    KHR_lights_punctual lights (Directional/Point only -- Spot falls back to Point,
//            Unknown is skipped), converted to world space and to the path tracer's radiance
//            scale (see RtLight's doc comment in PathTracer.h).
struct GltfBuildResult {
    std::vector<Phantom::RayTracer::RtTriangle> triangles;
    std::vector<Phantom::RayTracer::RtTexture>  textures;
    std::vector<Phantom::RayTracer::RtLight>    lights;
};

// Full conversion: triangles + textures + lights.
GltfBuildResult buildFromGltf(const Phantom::Gltf::GltfDocument& doc);

// Legacy: triangles only (textures discarded). All triangles are in world space.
std::vector<Phantom::RayTracer::RtTriangle> buildTrianglesFromGltf(const Phantom::Gltf::GltfDocument& doc);

// Extracts the document's first camera node (the same "first instance, depth-first" policy
// Universe's Renderer::applyAssetCamera() uses) as an RtCameraSpec. Only Perspective cameras are
// supported -- RtCameraSpec/PathTracer::Camera has no orthographic projection mode -- so an
// Orthographic-only document, or one with no camera at all, returns std::nullopt.
std::optional<Phantom::RayTracer::RtCameraSpec> extractFirstCamera(const Phantom::Gltf::GltfDocument& doc);
