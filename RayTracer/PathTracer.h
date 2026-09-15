#pragma once

#include "../../CGLib/Graphics/Image.h"
#include "../../CGLib/Math/Vector3d.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Phantom::RayTracer {

struct RenderSettings {
    int width = 256;
    int height = 256;
    int samplesPerPixel = 12;
    int maxDepth = 8;
    std::uint32_t randomSeed = 20260413u;
};

struct RtCameraSpec {
    double lookFrom[3] = {278.0, 278.0, -800.0};
    double lookAt[3]   = {278.0, 278.0,    0.0};
    double up[3]       = {  0.0,   1.0,    0.0};
    double fovDeg      = 40.0;
};

// Texture image data passed alongside triangles. pixels is always RGBA 8-bit.
struct RtTexture {
    std::vector<std::uint8_t> pixels;
    int width    = 0;
    int height   = 0;
    int channels = 4;
};

// A light imported from a glTF KHR_lights_punctual node (see GltfSceneBuilder::buildFromGltf()).
// Spot lights are not modeled -- their cone falls back to an omnidirectional Point light.
// `intensity` is expected to already be converted into the path tracer's unitless radiance
// scale by the caller (GltfSceneBuilder divides the glTF photometric intensity by the same
// 683 lm/W luminous-efficacy constant Universe's Rendering/GltfRenderer.cpp uses, so a scene
// lit by the same glTF file receives comparable light energy in both renderers).
struct RtLight {
    enum class Type { Directional, Point };
    Type   type         = Type::Directional;
    double position[3]  = {0.0, 0.0, 0.0};   // Point only
    double direction[3] = {0.0, -1.0, 0.0};  // Directional: direction the light travels
    double color[3]     = {1.0, 1.0, 1.0};
    double intensity    = 1.0;
};

struct RtTriangle {
    double v0[3]       = {0.0, 0.0, 0.0};
    double v1[3]       = {0.0, 0.0, 0.0};
    double v2[3]       = {0.0, 0.0, 0.0};
    double albedo[3]   = {0.8, 0.8, 0.8};
    double metallic    = 0.0;
    double roughness   = 0.5;
    double emission[3] = {0.0, 0.0, 0.0};

    // Per-vertex UV coordinates (texture space)
    double uv0[2] = {0.0, 0.0};
    double uv1[2] = {0.0, 0.0};
    double uv2[2] = {0.0, 0.0};

    // Indices into the RtTexture array passed to render(). -1 = no texture.
    int baseColorTextureIndex         = -1;
    int metallicRoughnessTextureIndex = -1;
    int normalTextureIndex            = -1;
    int emissiveTextureIndex          = -1;
};

struct SceneRenderResult {
    bool        ok = false;
    std::string outputPath;
    std::string error;
};

// ---------------------------------------------------------------------------
// PathTracer — class-based ray tracer API
// ---------------------------------------------------------------------------
class PathTracer {
public:
    explicit PathTracer(const RenderSettings& settings = {});

    void                 setSettings(const RenderSettings& settings);
    const RenderSettings& getSettings() const;

    // Render Cornell Box with default camera
    bool render(Graphics::Imageuc& output) const;

    // Render Cornell Box with custom camera
    bool render(const RtCameraSpec& cam, Graphics::Imageuc& output) const;

    // Render a triangle scene (e.g. from glTF)
    bool render(const std::vector<RtTriangle>& triangles,
                const RtCameraSpec& cam,
                Graphics::Imageuc& output) const;

    // Render a triangle scene with texture data
    bool render(const std::vector<RtTriangle>& triangles,
                const RtCameraSpec& cam,
                Graphics::Imageuc& output,
                const std::vector<RtTexture>& textures) const;

    // Render a triangle scene with texture data and explicit (KHR_lights_punctual) lights.
    // Lights are evaluated via next-event estimation against each surface's diffuse lobe only
    // (see PbrMaterial::directLightBRDF() in PathTracer.cpp) -- indirect/GI lighting and
    // emissive-triangle lighting are unaffected and still work exactly as before.
    bool render(const std::vector<RtTriangle>& triangles,
                const RtCameraSpec& cam,
                Graphics::Imageuc& output,
                const std::vector<RtTexture>& textures,
                const std::vector<RtLight>& lights) const;

    // Load and render a .cscene JSON file
    SceneRenderResult renderScene(const std::string& scenePath,
                                  Graphics::Imageuc& output) const;

private:
    RenderSettings settings_;
};

// ---------------------------------------------------------------------------
// Legacy free functions (deprecated — prefer PathTracer class)
// ---------------------------------------------------------------------------
[[deprecated("Use PathTracer::render() instead")]]
bool renderCornellBox(const RenderSettings& settings, Graphics::Imageuc& output);

[[deprecated("Use PathTracer::render(cam, output) instead")]]
bool renderCornellBoxWithCamera(const RenderSettings& settings,
                                const RtCameraSpec& cam,
                                Graphics::Imageuc& output);

[[deprecated("Use PathTracer::render(triangles, cam, output) instead")]]
bool renderTriangleScene(const std::vector<RtTriangle>& triangles,
                         const RtCameraSpec& cam,
                         const RenderSettings& settings,
                         Graphics::Imageuc& output);

[[deprecated("Use PathTracer::renderScene() instead")]]
SceneRenderResult renderScene(const std::string& scenePath, Graphics::Imageuc& output);

} // namespace Phantom::RayTracer
