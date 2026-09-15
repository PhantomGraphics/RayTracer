#include "pch.h"
#include "GltfSceneBuilder.h"

#include "../../CGLib/GltfRenderer/Gltf/GltfAccessorView.h"
#include "../../CGLib/GltfRenderer/Gltf/GltfTypes.h"
#include "../../CGLib/GltfRenderer/Gltf/GltfLightsCameras.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

using Phantom::RayTracer::RtTriangle;
using Phantom::RayTracer::RtTexture;
using Phantom::RayTracer::RtLight;
using Phantom::RayTracer::RtCameraSpec;
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

    // ---- lights (KHR_lights_punctual) ----
    const auto lc = collectGltfLightsAndCameras(doc);
    result.lights.reserve(lc.lights.size());
    for (const auto& inst : lc.lights) {
        const GltfLight& gl = doc.lights[inst.lightIndex];
        if (gl.type != "Directional" && gl.type != "Point" && gl.type != "Spot")
            continue; // "Unknown" -- nothing sensible to convert

        RtLight rl;
        // Spot falls back to omnidirectional Point -- RtLight has no cone attenuation yet.
        rl.type = (gl.type == "Directional") ? RtLight::Type::Directional : RtLight::Type::Point;

        const glm::vec3 pos = glm::vec3(inst.worldMatrix[3]);
        const glm::vec3 dir = glm::normalize(glm::vec3(inst.worldMatrix * glm::vec4(0.f, 0.f, -1.f, 0.f)));
        rl.position[0]  = pos.x;  rl.position[1]  = pos.y;  rl.position[2]  = pos.z;
        rl.direction[0] = dir.x;  rl.direction[1] = dir.y;  rl.direction[2] = dir.z;
        rl.color[0] = gl.color.r; rl.color[1] = gl.color.g; rl.color[2] = gl.color.b;
        // Same 683 lm/W luminous-efficacy conversion Universe's Rendering/GltfRenderer.cpp uses
        // (see its comment for the derivation) -- keeps the two renderers' light energy for the
        // same glTF file directly comparable, per Phase 4B item 5.
        rl.intensity = static_cast<double>(gl.intensity) / 683.0;
        result.lights.push_back(rl);
    }

    return result;
}

std::vector<RtTriangle> buildTrianglesFromGltf(const GltfDocument& doc)
{
    return buildFromGltf(doc).triangles;
}

std::optional<RtCameraSpec> extractFirstCamera(const GltfDocument& doc)
{
    const auto lc = collectGltfLightsAndCameras(doc);
    if (lc.cameras.empty()) return std::nullopt;

    // First instance found by the same depth-first node walk Universe's applyAssetCamera() uses.
    const auto& inst = lc.cameras.front();
    if (inst.cameraIndex < 0 || inst.cameraIndex >= static_cast<int>(doc.cameras.size()))
        return std::nullopt;
    const GltfCamera& cam = doc.cameras[inst.cameraIndex];
    if (cam.type != "Perspective") return std::nullopt; // RtCameraSpec has no ortho projection

    const glm::vec3 eye = glm::vec3(inst.worldMatrix[3]);
    // A glTF camera node looks along local -Z with +Y up (GltfLightsCameras.h doc comment).
    const glm::vec3 fwd = glm::normalize(glm::vec3(inst.worldMatrix * glm::vec4(0.f, 0.f, -1.f, 0.f)));
    const glm::vec3 up  = glm::normalize(glm::vec3(inst.worldMatrix * glm::vec4(0.f, 1.f, 0.f, 0.f)));

    RtCameraSpec spec;
    spec.lookFrom[0] = eye.x; spec.lookFrom[1] = eye.y; spec.lookFrom[2] = eye.z;
    const glm::vec3 at = eye + fwd;
    spec.lookAt[0] = at.x; spec.lookAt[1] = at.y; spec.lookAt[2] = at.z;
    spec.up[0] = up.x; spec.up[1] = up.y; spec.up[2] = up.z;
    spec.fovDeg = glm::degrees(cam.yfov);
    return spec;
}
