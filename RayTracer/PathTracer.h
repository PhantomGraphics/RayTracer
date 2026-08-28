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
