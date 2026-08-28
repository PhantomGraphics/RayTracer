#pragma once

#include <filesystem>
#include <vector>
#include "PathTracer.h"
#include "GLTFFile.h"

struct GltfConvertResult {
    std::vector<Phantom::RayTracer::RtTriangle> triangles;
    std::vector<Phantom::RayTracer::RtTexture>  textures;
};

// Convert a GLTFFile (loaded via Phantom::File::GLTFFileReader) to PathTracer input.
// basePath: parent directory of the source .gltf file, used to resolve external URI images.
// For .glb files with embedded images, basePath may be empty.
GltfConvertResult convertGltfToRt(const Phantom::File::GLTFFile& gltf,
                                   const std::filesystem::path&   basePath = {});
