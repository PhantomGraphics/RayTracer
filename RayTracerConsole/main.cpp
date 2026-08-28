#include "pch.h"
#include "PathTracer.h"
#include "GltfToRt.h"

#include "../../CGLib/Graphics/ImageFileWriter.h"
#include "GLTFFileReader.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sstream>

namespace {

int parseOrDefault(const char* text, const int fallback)
{
    if (!text) return fallback;
    int out = fallback;
    const auto len = std::strlen(text);
    const auto [ptr, ec] = std::from_chars(text, text + len, out);
    return (ec == std::errc{}) ? out : fallback;
}

double parseDoubleOrDefault(const char* text, const double fallback)
{
    if (!text) return fallback;
    double out = fallback;
    const auto len = std::strlen(text);
    const auto [ptr, ec] = std::from_chars(text, text + len, out);
    return (ec == std::errc{}) ? out : fallback;
}

bool hasExtension(const std::string& path, const std::string& ext)
{
    if (path.size() < ext.size()) return false;
    return path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

bool isGltfFile(const std::string& path)
{
    return hasExtension(path, ".glb") || hasExtension(path, ".gltf");
}

// Parse "x,y,z" into out[3]. Returns true on success.
bool parseVec3(const char* text, double out[3])
{
    if (!text) return false;
    char sep0 = 0, sep1 = 0;
    std::istringstream ss(text);
    ss >> out[0] >> sep0 >> out[1] >> sep1 >> out[2];
    return !ss.fail() && sep0 == ',' && sep1 == ',';
}

} // namespace

// ---------------------------------------------------------------------------
// --verify-texture: programmatic checkerboard scene for texture sanity check
// ---------------------------------------------------------------------------

namespace {

Phantom::RayTracer::RtTexture makeCheckerboard()
{
    Phantom::RayTracer::RtTexture tex;
    tex.width = 64; tex.height = 64; tex.channels = 4;
    tex.pixels.resize(64 * 64 * 4);
    for (int py = 0; py < 64; ++py) {
        for (int px = 0; px < 64; ++px) {
            const bool odd = ((px / 8) + (py / 8)) % 2 != 0;
            const std::uint8_t r = odd ? 255 : 220;
            const std::uint8_t g = odd ?   0 : 220;
            const std::uint8_t b = odd ?   0 : 220;
            const int i = (py * 64 + px) * 4;
            tex.pixels[i+0] = r; tex.pixels[i+1] = g;
            tex.pixels[i+2] = b; tex.pixels[i+3] = 255;
        }
    }
    return tex;
}

int runVerifyTexture(const std::string& outPath)
{
    auto makeFloorTri = [](
        double x0, double z0, double u0, double v0,
        double x1, double z1, double u1, double v1,
        double x2, double z2, double u2, double v2) -> Phantom::RayTracer::RtTriangle
    {
        Phantom::RayTracer::RtTriangle t{};
        t.albedo[0] = t.albedo[1] = t.albedo[2] = 1.0;
        t.roughness = 1.0; t.metallic = 0.0;
        t.baseColorTextureIndex = 0;
        t.v0[0]=x0; t.v0[1]=0.0; t.v0[2]=z0;
        t.v1[0]=x1; t.v1[1]=0.0; t.v1[2]=z1;
        t.v2[0]=x2; t.v2[1]=0.0; t.v2[2]=z2;
        t.uv0[0]=u0; t.uv0[1]=v0;
        t.uv1[0]=u1; t.uv1[1]=v1;
        t.uv2[0]=u2; t.uv2[1]=v2;
        return t;
    };

    const auto t0 = makeFloorTri(-3,-1, 0,0,  3,-1, 1,0,  3,5, 1,1);
    const auto t1 = makeFloorTri(-3,-1, 0,0,  3, 5, 1,1, -3,5, 0,1);

    Phantom::RayTracer::RtCameraSpec cam;
    cam.lookFrom[0] =  0.0; cam.lookFrom[1] = 4.0; cam.lookFrom[2] = -3.0;
    cam.lookAt[0]   =  0.0; cam.lookAt[1]   = 0.0; cam.lookAt[2]   =  2.0;
    cam.up[0] = 0.0; cam.up[1] = 1.0; cam.up[2] = 0.0;
    cam.fovDeg = 50.0;

    Phantom::RayTracer::RenderSettings settings;
    settings.width           = 256;
    settings.height          = 256;
    settings.samplesPerPixel = 32;
    settings.maxDepth        = 4;

    Phantom::Graphics::Imageuc image;
    Phantom::RayTracer::PathTracer tracer(settings);
    if (!tracer.render({t0, t1}, cam, image, {makeCheckerboard()})) {
        std::cerr << "--verify-texture: render failed.\n";
        return 1;
    }

    Phantom::Graphics::ImageFileWriter writer;
    if (!writer.write(outPath, image)) {
        std::cerr << "--verify-texture: cannot write " << outPath << "\n";
        return 1;
    }

    std::cout << outPath << std::endl;
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// glTF mode: load .glb / .gltf, convert, and path-trace
// ---------------------------------------------------------------------------

namespace {

int runGltf(const std::filesystem::path& modelPath, int argc, char** argv, int optStart)
{
    // Defaults for glTF mode
    Phantom::RayTracer::RenderSettings settings;
    settings.width           = 512;
    settings.height          = 512;
    settings.samplesPerPixel = 64;
    settings.maxDepth        = 8;

    std::string outPath = modelPath.stem().string() + ".png";

    Phantom::RayTracer::RtCameraSpec cam;
    cam.up[0] = 0.0; cam.up[1] = 1.0; cam.up[2] = 0.0;
    cam.fovDeg = 45.0;
    bool hasLookFrom = false;
    bool hasLookAt   = false;

    // Parse options starting at optStart
    for (int i = optStart; i < argc; ++i) {
        const std::string arg = argv[i];
        if      (arg == "--out"    && i+1 < argc) { outPath = argv[++i]; }
        else if (arg == "--width"  && i+1 < argc) { settings.width           = parseOrDefault(argv[++i], settings.width); }
        else if (arg == "--height" && i+1 < argc) { settings.height          = parseOrDefault(argv[++i], settings.height); }
        else if (arg == "--spp"    && i+1 < argc) { settings.samplesPerPixel = parseOrDefault(argv[++i], settings.samplesPerPixel); }
        else if (arg == "--depth"  && i+1 < argc) { settings.maxDepth        = parseOrDefault(argv[++i], settings.maxDepth); }
        else if (arg == "--from"   && i+1 < argc) { hasLookFrom = parseVec3(argv[++i], cam.lookFrom); }
        else if (arg == "--at"     && i+1 < argc) { hasLookAt   = parseVec3(argv[++i], cam.lookAt);   }
        else if (arg == "--fov"    && i+1 < argc) { cam.fovDeg = parseDoubleOrDefault(argv[++i], cam.fovDeg); }
    }

    // Load glTF / GLB
    Phantom::File::GLTFFileReader reader;
    if (!reader.read(modelPath)) {
        std::cerr << "Failed to load: " << modelPath << "\n";
        return 1;
    }

    // Convert to PathTracer format
    const auto built = convertGltfToRt(reader.getGLTF(), modelPath.parent_path());
    if (built.triangles.empty()) {
        std::cerr << "No triangles found in " << modelPath.filename() << "\n";
        return 1;
    }

    // Compute scene AABB for camera auto-placement
    if (!hasLookFrom || !hasLookAt) {
        double aabbMin[3] = { 1e30,  1e30,  1e30 };
        double aabbMax[3] = {-1e30, -1e30, -1e30 };
        for (const auto& tri : built.triangles) {
            const double* verts[3] = { tri.v0, tri.v1, tri.v2 };
            for (const double* v : verts)
                for (int k = 0; k < 3; ++k) {
                    aabbMin[k] = std::min(aabbMin[k], v[k]);
                    aabbMax[k] = std::max(aabbMax[k], v[k]);
                }
        }
        const double cx = (aabbMin[0] + aabbMax[0]) * 0.5;
        const double cy = (aabbMin[1] + aabbMax[1]) * 0.5;
        const double cz = (aabbMin[2] + aabbMax[2]) * 0.5;
        const double dx = aabbMax[0] - aabbMin[0];
        const double dy = aabbMax[1] - aabbMin[1];
        const double dz = aabbMax[2] - aabbMin[2];
        const double diag = std::sqrt(dx*dx + dy*dy + dz*dz);

        if (!hasLookAt) {
            cam.lookAt[0] = cx; cam.lookAt[1] = cy; cam.lookAt[2] = cz;
        }
        if (!hasLookFrom) {
            cam.lookFrom[0] = cx;
            cam.lookFrom[1] = cy + diag * 0.3;
            cam.lookFrom[2] = cz - diag * 1.5;
        }
    }

    std::cout << "Rendering: " << modelPath.filename().string() << "\n"
              << "  Triangles: " << built.triangles.size()
              << ", Textures: "  << built.textures.size() << "\n"
              << "  Resolution: " << settings.width << "x" << settings.height
              << ", SPP: " << settings.samplesPerPixel
              << ", Depth: " << settings.maxDepth << "\n";

    Phantom::Graphics::Imageuc image;
    Phantom::RayTracer::PathTracer tracer(settings);
    if (!tracer.render(built.triangles, cam, image, built.textures)) {
        std::cerr << "Render failed.\n";
        return 1;
    }

    Phantom::Graphics::ImageFileWriter writer;
    if (!writer.write(outPath, image)) {
        std::cerr << "Cannot write output to " << outPath << "\n";
        return 1;
    }

    std::cout << outPath << std::endl;
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    // --verify-texture [output.png]
    if (argc > 1 && std::string(argv[1]) == "--verify-texture") {
        const std::string out = (argc > 2) ? argv[2] : "texture_verify.png";
        return runVerifyTexture(out);
    }

    // model.glb / model.gltf [options...]
    if (argc > 1 && isGltfFile(argv[1])) {
        return runGltf(argv[1], argc, argv, 2);
    }

    // scene.cscene
    if (argc > 1 && hasExtension(argv[1], ".cscene")) {
        Phantom::Graphics::Imageuc image;
        Phantom::RayTracer::PathTracer tracer;
        const auto result = tracer.renderScene(argv[1], image);
        if (!result.ok) {
            std::cerr << "Render failed: " << result.error << "\n";
            return 1;
        }

        Phantom::Graphics::ImageFileWriter writer;
        if (!writer.write(result.outputPath, image)) {
            std::cerr << "Render failed: cannot write " << result.outputPath << "\n";
            return 1;
        }

        std::cout << result.outputPath << "\n";
        return 0;
    }

    // Legacy: [width height spp depth output.png] → Cornell Box
    Phantom::RayTracer::RenderSettings settings;
    std::string outputPath = "cornell_box.png";

    if (argc > 1) settings.width           = parseOrDefault(argv[1], settings.width);
    if (argc > 2) settings.height          = parseOrDefault(argv[2], settings.height);
    if (argc > 3) settings.samplesPerPixel = parseOrDefault(argv[3], settings.samplesPerPixel);
    if (argc > 4) settings.maxDepth        = parseOrDefault(argv[4], settings.maxDepth);
    if (argc > 5) outputPath = argv[5];

    Phantom::Graphics::Imageuc image;
    Phantom::RayTracer::PathTracer tracer(settings);
    if (!tracer.render(image)) {
        std::cerr << "Render failed: invalid render settings.\n";
        return 1;
    }

    Phantom::Graphics::ImageFileWriter writer;
    if (!writer.write(outputPath, image)) {
        std::cerr << "Render failed: cannot write " << outputPath << "\n";
        return 1;
    }

    std::cout << "Rendered Cornell Box to " << outputPath << "\n"
              << "Resolution: " << settings.width << "x" << settings.height
              << ", SPP: " << settings.samplesPerPixel
              << ", MaxDepth: " << settings.maxDepth << "\n";
    return 0;
}
