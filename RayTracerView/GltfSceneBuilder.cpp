#include "pch.h"
#include "GltfSceneBuilder.h"

#include "../../CGLib/GltfRenderer/Gltf/GltfAccessorView.h"
#include "../../CGLib/GltfRenderer/Gltf/GltfTypes.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

using Phantom::RayTracer::RtTriangle;
using Phantom::RayTracer::RtTexture;
using namespace Phantom::Gltf;

namespace {

glm::mat4 nodeLocalTransform(const GltfNode& node)
{
    if (node.hasMatrix) return node.matrix;
    const glm::mat4 T = glm::translate(glm::mat4(1.f), node.translation);
    const glm::quat q(node.rotation.w, node.rotation.x,
                      node.rotation.y, node.rotation.z);
    const glm::mat4 R = glm::mat4_cast(q);
    const glm::mat4 S = glm::scale(glm::mat4(1.f), node.scale);
    return T * R * S;
}

// Extract all triangles from one node (and recursively its children)
void extractTriangles(const GltfDocument& doc,
                      int                 nodeIndex,
                      const glm::mat4&    parentWorld,
                      std::vector<RtTriangle>& out)
{
    if (nodeIndex < 0 || nodeIndex >= static_cast<int>(doc.nodes.size())) return;

    const GltfNode& node  = doc.nodes[nodeIndex];
    const glm::mat4 world = parentWorld * nodeLocalTransform(node);

    if (node.meshIndex >= 0 && node.meshIndex < static_cast<int>(doc.meshes.size())) {
        const GltfMesh& mesh = doc.meshes[node.meshIndex];

        for (const GltfPrimitive& prim : mesh.primitives) {
            if (prim.positionAccessor < 0) continue;

            // ---- positions (float vec3 -> world space) ----
            GltfAccessorView posView(doc, prim.positionAccessor);
            std::vector<glm::vec3> pos;
            pos.reserve(posView.count());
            for (size_t i = 0; i < posView.count(); ++i) {
                const glm::vec3 lp = posView.get<glm::vec3>(i);
                const glm::vec4 wp = world * glm::vec4(lp, 1.f);
                pos.emplace_back(wp.x, wp.y, wp.z);
            }

            // ---- material (glTF PBR 竊・RtTriangle defaults) ----
            RtTriangle tpl{};
            tpl.albedo[0] = 0.8; tpl.albedo[1] = 0.8; tpl.albedo[2] = 0.8;
            tpl.metallic  = 0.0;
            tpl.roughness = 0.5;

            if (prim.materialIndex >= 0 &&
                prim.materialIndex < static_cast<int>(doc.materials.size()))
            {
                const GltfMaterial& mat = doc.materials[prim.materialIndex];
                const auto& pbr = mat.pbrMetallicRoughness;
                tpl.albedo[0] = static_cast<double>(pbr.baseColorFactor.r);
                tpl.albedo[1] = static_cast<double>(pbr.baseColorFactor.g);
                tpl.albedo[2] = static_cast<double>(pbr.baseColorFactor.b);
                tpl.metallic  = static_cast<double>(pbr.metallicFactor);
                tpl.roughness = static_cast<double>(pbr.roughnessFactor);
                tpl.emission[0] = static_cast<double>(mat.emissiveFactor.r);
                tpl.emission[1] = static_cast<double>(mat.emissiveFactor.g);
                tpl.emission[2] = static_cast<double>(mat.emissiveFactor.b);
                // Texture indices reference GltfDocument::textures[], which maps
                // to GltfBuildResult::textures[] built in buildFromGltf().
                tpl.baseColorTextureIndex         = pbr.baseColorTexture.index;
                tpl.metallicRoughnessTextureIndex  = pbr.metallicRoughnessTexture.index;
                tpl.emissiveTextureIndex           = mat.emissiveTexture.index;
            }

            // ---- UV coordinates (texCoord0) ----
            std::vector<glm::vec2> uvs;
            if (prim.texCoord0Accessor >= 0) {
                GltfAccessorView uvView(doc, prim.texCoord0Accessor);
                uvs.reserve(uvView.count());
                for (size_t i = 0; i < uvView.count(); ++i)
                    uvs.push_back(uvView.get<glm::vec2>(i));
            }

            // ---- emit one RtTriangle ----
            auto emit = [&](uint32_t i0, uint32_t i1, uint32_t i2) {
                if (i0 >= pos.size() || i1 >= pos.size() || i2 >= pos.size()) return;
                RtTriangle tri = tpl;
                tri.v0[0] = pos[i0].x; tri.v0[1] = pos[i0].y; tri.v0[2] = pos[i0].z;
                tri.v1[0] = pos[i1].x; tri.v1[1] = pos[i1].y; tri.v1[2] = pos[i1].z;
                tri.v2[0] = pos[i2].x; tri.v2[1] = pos[i2].y; tri.v2[2] = pos[i2].z;
                if (i0 < uvs.size() && i1 < uvs.size() && i2 < uvs.size()) {
                    tri.uv0[0] = uvs[i0].x; tri.uv0[1] = uvs[i0].y;
                    tri.uv1[0] = uvs[i1].x; tri.uv1[1] = uvs[i1].y;
                    tri.uv2[0] = uvs[i2].x; tri.uv2[1] = uvs[i2].y;
                }
                out.push_back(tri);
            };

            // ---- index buffer ----
            if (prim.indicesAccessor >= 0) {
                GltfAccessorView idxView(doc, prim.indicesAccessor);
                const GltfComponentType ct =
                    doc.accessors[prim.indicesAccessor].componentType;
                const size_t triCount = idxView.count() / 3;
                out.reserve(out.size() + triCount);

                for (size_t t = 0; t < triCount; ++t) {
                    uint32_t i0, i1, i2;
                    if (ct == GltfComponentType::UnsignedShort) {
                        i0 = idxView.get<uint16_t>(t * 3 + 0);
                        i1 = idxView.get<uint16_t>(t * 3 + 1);
                        i2 = idxView.get<uint16_t>(t * 3 + 2);
                    } else if (ct == GltfComponentType::UnsignedByte) {
                        i0 = idxView.get<uint8_t>(t * 3 + 0);
                        i1 = idxView.get<uint8_t>(t * 3 + 1);
                        i2 = idxView.get<uint8_t>(t * 3 + 2);
                    } else {
                        i0 = idxView.get<uint32_t>(t * 3 + 0);
                        i1 = idxView.get<uint32_t>(t * 3 + 1);
                        i2 = idxView.get<uint32_t>(t * 3 + 2);
                    }
                    emit(i0, i1, i2);
                }
            } else {
                // Non-indexed: every 3 consecutive vertices form a triangle
                const size_t triCount = pos.size() / 3;
                out.reserve(out.size() + triCount);
                for (size_t t = 0; t < triCount; ++t)
                    emit(static_cast<uint32_t>(t * 3),
                         static_cast<uint32_t>(t * 3 + 1),
                         static_cast<uint32_t>(t * 3 + 2));
            }
        }
    }

    for (int child : node.children)
        extractTriangles(doc, child, world, out);
}

} // namespace

GltfBuildResult buildFromGltf(const GltfDocument& doc)
{
    GltfBuildResult result;
    if (doc.scenes.empty()) return result;

    const int sceneIdx = (doc.defaultScene >= 0 &&
                          doc.defaultScene < static_cast<int>(doc.scenes.size()))
                         ? doc.defaultScene : 0;

    for (int nodeIdx : doc.scenes[sceneIdx].nodes)
        extractTriangles(doc, nodeIdx, glm::mat4(1.f), result.triangles);

    // Build one RtTexture per GltfTexture entry so that
    // RtTriangle::*TextureIndex directly indexes this list.
    result.textures.reserve(doc.textures.size());
    for (const auto& gltfTex : doc.textures) {
        RtTexture tex;
        if (gltfTex.imageIndex >= 0 &&
            gltfTex.imageIndex < static_cast<int>(doc.images.size()))
        {
            const GltfImage& img = doc.images[gltfTex.imageIndex];
            tex.pixels   = img.pixels;
            tex.width    = img.width;
            tex.height   = img.height;
            tex.channels = img.channels;
        }
        result.textures.push_back(std::move(tex));
    }

    return result;
}

std::vector<RtTriangle> buildTrianglesFromGltf(const GltfDocument& doc)
{
    return buildFromGltf(doc).triangles;
}
