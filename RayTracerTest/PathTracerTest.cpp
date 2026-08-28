#include "pch.h"
#include "../RayTracer/PathTracer.h"

// ---------------------------------------------------------------------------
// Group 1: Struct default values
// ---------------------------------------------------------------------------

TEST(RenderSettingsTest, DefaultValues)
{
    Phantom::RayTracer::RenderSettings s;
    EXPECT_EQ(s.width,          256);
    EXPECT_EQ(s.height,         256);
    EXPECT_EQ(s.samplesPerPixel, 12);
    EXPECT_EQ(s.maxDepth,         8);
}

TEST(RtCameraSpecTest, DefaultValues)
{
    Phantom::RayTracer::RtCameraSpec cam;
    EXPECT_DOUBLE_EQ(cam.lookFrom[0], 278.0);
    EXPECT_DOUBLE_EQ(cam.lookFrom[1], 278.0);
    EXPECT_DOUBLE_EQ(cam.lookFrom[2], -800.0);
    EXPECT_DOUBLE_EQ(cam.lookAt[0],  278.0);
    EXPECT_DOUBLE_EQ(cam.lookAt[1],  278.0);
    EXPECT_DOUBLE_EQ(cam.lookAt[2],    0.0);
    EXPECT_DOUBLE_EQ(cam.up[1],        1.0);
    EXPECT_DOUBLE_EQ(cam.fovDeg,      40.0);
}

TEST(RtTextureTest, DefaultValues)
{
    Phantom::RayTracer::RtTexture tex;
    EXPECT_EQ(tex.width,    0);
    EXPECT_EQ(tex.height,   0);
    EXPECT_EQ(tex.channels, 4);
    EXPECT_TRUE(tex.pixels.empty());
}

TEST(RtTextureTest, PixelStorage)
{
    Phantom::RayTracer::RtTexture tex;
    tex.width    = 2;
    tex.height   = 2;
    tex.channels = 4;
    // 2x2: red, green, blue, yellow
    tex.pixels = {
        255, 0,   0,   255,
        0,   255, 0,   255,
        0,   0,   255, 255,
        255, 255, 0,   255
    };
    EXPECT_EQ(tex.pixels.size(), 16u);
    EXPECT_EQ(tex.pixels[0], 255); // R of pixel (0,0)
    EXPECT_EQ(tex.pixels[4], 0);   // R of pixel (1,0)
}

TEST(RtTriangleTest, DefaultValues)
{
    Phantom::RayTracer::RtTriangle tri;
    EXPECT_DOUBLE_EQ(tri.albedo[0],  0.8);
    EXPECT_DOUBLE_EQ(tri.albedo[1],  0.8);
    EXPECT_DOUBLE_EQ(tri.albedo[2],  0.8);
    EXPECT_DOUBLE_EQ(tri.metallic,   0.0);
    EXPECT_DOUBLE_EQ(tri.roughness,  0.5);
    EXPECT_DOUBLE_EQ(tri.emission[0], 0.0);
}

TEST(RtTriangleTest, UVDefaultValues)
{
    Phantom::RayTracer::RtTriangle tri;
    EXPECT_DOUBLE_EQ(tri.uv0[0], 0.0);
    EXPECT_DOUBLE_EQ(tri.uv0[1], 0.0);
    EXPECT_DOUBLE_EQ(tri.uv1[0], 0.0);
    EXPECT_DOUBLE_EQ(tri.uv1[1], 0.0);
    EXPECT_DOUBLE_EQ(tri.uv2[0], 0.0);
    EXPECT_DOUBLE_EQ(tri.uv2[1], 0.0);
    EXPECT_EQ(tri.baseColorTextureIndex,         -1);
    EXPECT_EQ(tri.metallicRoughnessTextureIndex, -1);
    EXPECT_EQ(tri.normalTextureIndex,            -1);
    EXPECT_EQ(tri.emissiveTextureIndex,          -1);
}

// ---------------------------------------------------------------------------
// Group 2: PathTracer class construction and settings
// ---------------------------------------------------------------------------

TEST(PathTracerTest, ConstructWithDefaultSettings)
{
    Phantom::RayTracer::PathTracer tracer;
    const auto& s = tracer.getSettings();
    EXPECT_EQ(s.width,  256);
    EXPECT_EQ(s.height, 256);
}

TEST(PathTracerTest, ConstructWithCustomSettings)
{
    Phantom::RayTracer::RenderSettings settings;
    settings.width          = 128;
    settings.height         = 64;
    settings.samplesPerPixel = 4;
    settings.maxDepth       = 3;

    Phantom::RayTracer::PathTracer tracer(settings);
    const auto& s = tracer.getSettings();
    EXPECT_EQ(s.width,           128);
    EXPECT_EQ(s.height,           64);
    EXPECT_EQ(s.samplesPerPixel,   4);
    EXPECT_EQ(s.maxDepth,          3);
}

TEST(PathTracerTest, SetAndGetSettings)
{
    Phantom::RayTracer::PathTracer tracer;

    Phantom::RayTracer::RenderSettings settings;
    settings.width  = 512;
    settings.height = 512;
    tracer.setSettings(settings);

    EXPECT_EQ(tracer.getSettings().width,  512);
    EXPECT_EQ(tracer.getSettings().height, 512);
}

// ---------------------------------------------------------------------------
// Group 3: Rendering (low resolution for speed)
// ---------------------------------------------------------------------------
/*
TEST(PathTracerTest, RenderCornellBoxSmall)
{
    Phantom::RayTracer::RenderSettings settings;
    settings.width          = 32;
    settings.height         = 32;
    settings.samplesPerPixel = 1;
    settings.maxDepth       = 2;

    Phantom::RayTracer::PathTracer tracer(settings);
    Phantom::Graphics::Imageuc output;
    const bool ok = tracer.render(output);

    EXPECT_TRUE(ok);
    EXPECT_EQ(output.getWidth(),  32);
    EXPECT_EQ(output.getHeight(), 32);
}

TEST(PathTracerTest, RenderCornellBoxWithCamera)
{
    Phantom::RayTracer::RenderSettings settings;
    settings.width          = 32;
    settings.height         = 32;
    settings.samplesPerPixel = 1;
    settings.maxDepth       = 2;

    Phantom::RayTracer::RtCameraSpec cam;
    // default camera spec (Cornell Box view)

    Phantom::RayTracer::PathTracer tracer(settings);
    Phantom::Graphics::Imageuc output;
    const bool ok = tracer.render(cam, output);

    EXPECT_TRUE(ok);
    EXPECT_EQ(output.getWidth(),  32);
    EXPECT_EQ(output.getHeight(), 32);
}
*/
TEST(PathTracerTest, RenderSingleTriangle)
{
    Phantom::RayTracer::RenderSettings settings;
    settings.width          = 16;
    settings.height         = 16;
    settings.samplesPerPixel = 1;
    settings.maxDepth       = 2;

    Phantom::RayTracer::RtTriangle tri;
    tri.v0[0] = -100.0; tri.v0[1] = -100.0; tri.v0[2] = 0.0;
    tri.v1[0] =  100.0; tri.v1[1] = -100.0; tri.v1[2] = 0.0;
    tri.v2[0] =    0.0; tri.v2[1] =  100.0; tri.v2[2] = 0.0;
    tri.albedo[0] = 1.0; tri.albedo[1] = 0.5; tri.albedo[2] = 0.0;

    Phantom::RayTracer::RtCameraSpec cam;
    cam.lookFrom[0] = 0.0; cam.lookFrom[1] = 0.0; cam.lookFrom[2] = -500.0;
    cam.lookAt[0]   = 0.0; cam.lookAt[1]   = 0.0; cam.lookAt[2]   =    0.0;
    cam.up[0] = 0.0; cam.up[1] = 1.0; cam.up[2] = 0.0;
    cam.fovDeg = 45.0;

    Phantom::RayTracer::PathTracer tracer(settings);
    Phantom::Graphics::Imageuc output;
    const bool ok = tracer.render({tri}, cam, output);

    EXPECT_TRUE(ok);
    EXPECT_EQ(output.getWidth(),  16);
    EXPECT_EQ(output.getHeight(), 16);
}

TEST(PathTracerTest, RenderEmptyTriangleScene)
{
    Phantom::RayTracer::RenderSettings settings;
    settings.width          = 16;
    settings.height         = 16;
    settings.samplesPerPixel = 1;
    settings.maxDepth       = 2;

    Phantom::RayTracer::RtCameraSpec cam;
    Phantom::RayTracer::PathTracer tracer(settings);
    Phantom::Graphics::Imageuc output;

    // Empty triangle list should not crash and should produce a sky image
    const bool ok = tracer.render({}, cam, output);

    EXPECT_TRUE(ok);
    EXPECT_EQ(output.getWidth(),  16);
    EXPECT_EQ(output.getHeight(), 16);
}

TEST(PathTracerTest, RenderWithTexturesDoesNotCrash)
{
    Phantom::RayTracer::RenderSettings settings;
    settings.width          = 8;
    settings.height         = 8;
    settings.samplesPerPixel = 1;
    settings.maxDepth       = 2;

    Phantom::RayTracer::RtTriangle tri;
    tri.v0[0] = -1.0; tri.v0[1] = -1.0; tri.v0[2] = 0.0;
    tri.v1[0] =  1.0; tri.v1[1] = -1.0; tri.v1[2] = 0.0;
    tri.v2[0] =  0.0; tri.v2[1] =  1.0; tri.v2[2] = 0.0;
    tri.uv0[0] = 0.0; tri.uv0[1] = 0.0;
    tri.uv1[0] = 1.0; tri.uv1[1] = 0.0;
    tri.uv2[0] = 0.5; tri.uv2[1] = 1.0;
    tri.baseColorTextureIndex = 0;

    Phantom::RayTracer::RtTexture tex;
    tex.width = 2; tex.height = 2; tex.channels = 4;
    tex.pixels = { 255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,0,255 };

    Phantom::RayTracer::RtCameraSpec cam;
    cam.lookFrom[0] = 0.0; cam.lookFrom[1] = 0.0; cam.lookFrom[2] = -5.0;
    cam.lookAt[0]   = 0.0; cam.lookAt[1]   = 0.0; cam.lookAt[2]   =  0.0;
    cam.fovDeg = 45.0;

    Phantom::RayTracer::PathTracer tracer(settings);
    Phantom::Graphics::Imageuc output;
    const bool ok = tracer.render({tri}, cam, output, {tex});

    EXPECT_TRUE(ok);
    EXPECT_EQ(output.getWidth(),  8);
    EXPECT_EQ(output.getHeight(), 8);
}

TEST(PathTracerTest, InvalidSettingsReturnFalse)
{
    Phantom::RayTracer::RenderSettings bad;
    bad.width          = -1;
    bad.samplesPerPixel = 0;

    Phantom::RayTracer::PathTracer tracer(bad);
    Phantom::Graphics::Imageuc output;
    EXPECT_FALSE(tracer.render(output));
}

// ---------------------------------------------------------------------------
// Group 4: renderScene file I/O
// (Working directory is set to $(SolutionDir)RayTracer\ via post-build copy)
// ---------------------------------------------------------------------------

/*
TEST(PathTracerTest, RenderSceneValidFile)
{
    Phantom::RayTracer::PathTracer tracer;
    Phantom::Graphics::Imageuc output;

    // cornell_box.cscene is copied to the output directory by post-build event
    const auto result = tracer.renderScene("cornell_box.cscene", output);
    EXPECT_TRUE(result.ok) << result.error;
    EXPECT_FALSE(result.outputPath.empty());
}
*/

TEST(PathTracerTest, RenderSceneInvalidPath)
{
    Phantom::RayTracer::PathTracer tracer;
    Phantom::Graphics::Imageuc output;

    const auto result = tracer.renderScene("nonexistent_scene.cscene", output);
    EXPECT_FALSE(result.ok);
    EXPECT_FALSE(result.error.empty());
}

// ---------------------------------------------------------------------------
// Test runner entry point
// ---------------------------------------------------------------------------

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
