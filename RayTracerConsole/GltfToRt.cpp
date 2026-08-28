#include "pch.h"
#include "GltfToRt.h"

#include "CGLib/Math/glm.h"
#include "CGLib/ThirdParty/glm-0.9.9.8/glm/gtc/matrix_transform.hpp"
#include "CGLib/ThirdParty/glm-0.9.9.8/glm/gtc/quaternion.hpp"

// stb_image implementation is compiled into Graphics.lib (ImageFileReader.cpp).
// Include the header only — do NOT define STB_IMAGE_IMPLEMENTATION here.
#include "stb_image.h"

using Phantom::RayTracer::RtTexture;
using Phantom::RayTracer::RtTriangle;
using Phantom::File::GLTFFile;
using Phantom::File::GLTFNode;
using Phantom::File::GLTFPrimitive;
using Phantom::File::GLTFPrimitiveMode;

namespace {

// Decode a GLTFImage to RGBA pixels.
// GLB-embedded images have raw compressed bytes in img.data.
// External .gltf images have a URI path in img.uri (relative to basePath).
RtTexture decodeImage(const Phantom::File::GLTFImage& img,
                      const std::filesystem::path&    basePath)
{
    RtTexture tex;
    int w = 0, h = 0, ch = 0;
    stbi_uc* px = nullptr;

    if (!img.data.empty()) {
        px = stbi_load_from_memory(
            img.data.data(), static_cast<int>(img.data.size()),
            &w, &h, &ch, 4);
    } else if (!img.uri.empty()) {
        const auto path = basePath / img.uri;
        px = stbi_load(path.string().c_str(), &w, &h, &ch, 4);
    }

    if (px) {
        tex.width    = w;
        tex.height   = h;
        tex.channels = 4;
        tex.pixels.assign(px, px + static_cast<size_t>(w) * h * 4);
        stbi_image_free(px);
    }
    return tex;
}

// Build a 4x4 local transform from a node's TRS components.
// GLTFNode::rotation stores [x, y, z, w]; glm::quat constructor takes (w, x, y, z).
glm::mat4 nodeLocalTransform(const GLTFNode& node)
{
    const glm::mat4 T = glm::translate(glm::mat4(1.f),
        glm::vec3(node.translation[0], node.translation[1], node.translation[2]));
    const glm::quat q(
        node.rotation[3], node.rotation[0],
        node.rotation[1], node.rotation[2]);
    const glm::mat4 R = glm::mat4_cast(q);
    const glm::mat4 S = glm::scale(glm::mat4(1.f),
        glm::vec3(node.scale[0], node.scale[1], node.scale[2]));
    return T * R * S;
}

// Convert one GLTFPrimitive to RtTriangle entries and append to out.
void emitTriangles(const GLTFPrimitive&        prim,
                   const GLTFFile&             gltf,
                   const glm::mat4&            world,
                   std::vector<RtTriangle>&    out)
{
    if (prim.mode != GLTFPrimitiveMode::Triangles) return;
    if (prim.positions.empty()) return;

    // Transform positions to world space.
    std::vector<glm::vec3> wpos;
    wpos.reserve(prim.positions.size());
    for (const auto& p : prim.positions) {
        const glm::vec4 wp = world * glm::vec4(p.x, p.y, p.z, 1.f);
        wpos.emplace_back(wp.x, wp.y, wp.z);
    }

    // Build triangle template from the primitive's PBR material.
    RtTriangle tpl{};
    tpl.albedo[0] = 0.8; tpl.albedo[1] = 0.8; tpl.albedo[2] = 0.8;
    tpl.metallic  = 0.0;
    tpl.roughness = 0.5;

    if (prim.materialIndex >= 0 &&
        prim.materialIndex < static_cast<int>(gltf.materials.size()))
    {
        const auto& mat = gltf.materials[prim.materialIndex];
        const auto& pbr = mat.pbrMetallicRoughness;
        tpl.albedo[0]   = static_cast<double>(pbr.baseColorFactor[0]);
        tpl.albedo[1]   = static_cast<double>(pbr.baseColorFactor[1]);
        tpl.albedo[2]   = static_cast<double>(pbr.baseColorFactor[2]);
        tpl.metallic    = static_cast<double>(pbr.metallicFactor);
        tpl.roughness   = static_cast<double>(pbr.roughnessFactor);
        tpl.emission[0] = static_cast<double>(mat.emissiveFactor[0]);
        tpl.emission[1] = static_cast<double>(mat.emissiveFactor[1]);
        tpl.emission[2] = static_cast<double>(mat.emissiveFactor[2]);
        tpl.baseColorTextureIndex         = pbr.baseColorTextureIndex;
        tpl.metallicRoughnessTextureIndex = pbr.metallicRoughnessTextureIndex;
        tpl.emissiveTextureIndex          = mat.emissiveTextureIndex;
    }

    const auto& uvs = prim.texCoords;

    auto emit = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
        if (i0 >= wpos.size() || i1 >= wpos.size() || i2 >= wpos.size()) return;
        RtTriangle tri = tpl;
        tri.v0[0] = wpos[i0].x; tri.v0[1] = wpos[i0].y; tri.v0[2] = wpos[i0].z;
        tri.v1[0] = wpos[i1].x; tri.v1[1] = wpos[i1].y; tri.v1[2] = wpos[i1].z;
        tri.v2[0] = wpos[i2].x; tri.v2[1] = wpos[i2].y; tri.v2[2] = wpos[i2].z;
        if (i0 < uvs.size() && i1 < uvs.size() && i2 < uvs.size()) {
            tri.uv0[0] = uvs[i0].x; tri.uv0[1] = uvs[i0].y;
            tri.uv1[0] = uvs[i1].x; tri.uv1[1] = uvs[i1].y;
            tri.uv2[0] = uvs[i2].x; tri.uv2[1] = uvs[i2].y;
        }
        out.push_back(tri);
    };

    if (!prim.indices.empty()) {
        const size_t triCount = prim.indices.size() / 3;
        out.reserve(out.size() + triCount);
        for (size_t t = 0; t < triCount; ++t)
            emit(prim.indices[t*3], prim.indices[t*3+1], prim.indices[t*3+2]);
    } else {
        const size_t triCount = wpos.size() / 3;
        out.reserve(out.size() + triCount);
        for (size_t t = 0; t < triCount; ++t)
            emit(static_cast<uint32_t>(t*3),
                 static_cast<uint32_t>(t*3+1),
                 static_cast<uint32_t>(t*3+2));
    }
}

// Recursively walk the node tree and accumulate triangles.
void extractNode(const GLTFFile&          gltf,
                 int                      nodeIndex,
                 const glm::mat4&         parentWorld,
                 std::vector<RtTriangle>& out)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(gltf.nodes.size())) return;
    const auto& node = gltf.nodes[nodeIndex];
    const glm::mat4 world = parentWorld * nodeLocalTransform(node);

    if (node.meshIndex >= 0 && node.meshIndex < static_cast<int>(gltf.meshes.size()))
        for (const auto& prim : gltf.meshes[node.meshIndex].primitives)
            emitTriangles(prim, gltf, world, out);

    for (int child : node.children)
        extractNode(gltf, child, world, out);
}

} // namespace

GltfConvertResult convertGltfToRt(const GLTFFile&              gltf,
                                   const std::filesystem::path& basePath)
{
    GltfConvertResult result;

    // Decode textures. result.textures[i] aligns 1:1 with gltf.textures[i]
    // so RtTriangle::*TextureIndex can index result.textures directly.
    result.textures.reserve(gltf.textures.size());
    for (const auto& tex : gltf.textures) {
        if (tex.imageIndex >= 0 &&
            tex.imageIndex < static_cast<int>(gltf.images.size()))
        {
            result.textures.push_back(decodeImage(gltf.images[tex.imageIndex], basePath));
        } else {
            result.textures.emplace_back(); // empty placeholder keeps index alignment
        }
    }

    // Traverse the default scene.
    if (gltf.scenes.empty()) return result;

    const int sceneIdx =
        (gltf.defaultScene >= 0 &&
         gltf.defaultScene < static_cast<int>(gltf.scenes.size()))
        ? gltf.defaultScene : 0;

    for (int nodeIdx : gltf.scenes[sceneIdx].nodes)
        extractNode(gltf, nodeIdx, glm::mat4(1.f), result.triangles);

    return result;
}
