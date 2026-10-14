#pragma once

#include <filesystem>
#include <vector>
#include "PathTracer.h"
#include "GltfFile.h"

struct GltfConvertResult {
    std::vector<Phantom::RayTracer::RtTriangle> triangles;
    std::vector<Phantom::RayTracer::RtTexture>  textures;
};

// Convert a GltfFile (loaded via Phantom::File::GltfFileReader) to PathTracer input.
// basePath: parent directory of the source .gltf file, used to resolve external URI images.
// For .glb files with embedded images, basePath may be empty.
GltfConvertResult convertGltfToRt(const Phantom::File::GltfFile& gltf,
                                   const std::filesystem::path&   basePath = {});
