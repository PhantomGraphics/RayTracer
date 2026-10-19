#include "pch.h"
#include "../Core/PathTracer.h"
#include "../Core/PointCloudScene.h"

#include <cmath>
#include <memory>
#include <random>
#include <vector>

using namespace Phantom::RayTracer;

namespace {

struct Cloud {
    std::vector<float> positions, colors, radii;
    std::shared_ptr<RtPointCloudData> data() {
        return std::make_shared<RtPointCloudData>(positions.data(), colors.empty() ? nullptr : colors.data(), radii.data(),
            radii.size(), std::shared_ptr<const void>());
    }
};

// Brute force: the nearest sphere hit, the reference for the BVH.
bool bruteForce(const Cloud& c, const double o[3], const double d[3], double tMin, double tMax, double scale, uint32_t& index, double& best)
{
    bool found = false;
    best = tMax;
    const double a = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    for (size_t i = 0; i < c.radii.size(); ++i) {
        const double oc[3] = { o[0] - c.positions[3 * i], o[1] - c.positions[3 * i + 1], o[2] - c.positions[3 * i + 2] };
        const double r = c.radii[i] * scale;
        const double hb = oc[0] * d[0] + oc[1] * d[1] + oc[2] * d[2];
        const double cc = oc[0] * oc[0] + oc[1] * oc[1] + oc[2] * oc[2] - r * r;
        const double disc = hb * hb - a * cc;
        if (disc < 0.0) continue;
        const double s = std::sqrt(disc);
        double t = (-hb - s) / a;
        if (!(t > tMin && t < best)) { t = (-hb + s) / a; if (!(t > tMin && t < best)) continue; }
        best = t; index = static_cast<uint32_t>(i); found = true;
    }
    return found;
}

Cloud randomCloud(size_t n, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> pos(-5.f, 5.f), rad(0.05f, 0.4f);
    Cloud c;
    for (size_t i = 0; i < n; ++i) {
        for (int k = 0; k < 3; ++k) c.positions.push_back(pos(rng));
        c.radii.push_back(rad(rng));
    }
    return c;
}

// A pixel is "red" when the sphere (albedo 1,0,0) is seen: the sky behind it is blue-white, so red clearly beats blue.
bool isRed(const Phantom::Graphics::Imageuc& image, int x, int y)
{
    const auto c = image.getColor(x, y);
    return c.r > 60 && c.r > c.b + 40;
}

RenderSettings smallSettings(int size = 48)
{
    RenderSettings s;
    s.width = s.height = size;
    s.samplesPerPixel = 4;
    s.maxDepth = 3;
    s.quiet = true;
    return s;
}

RtCameraSpec frontCamera()
{
    RtCameraSpec cam;
    cam.lookFrom[0] = 0; cam.lookFrom[1] = 0; cam.lookFrom[2] = 10;
    cam.lookAt[0] = 0; cam.lookAt[1] = 0; cam.lookAt[2] = 0;
    cam.fovDeg = 30.0;
    return cam;
}

RtPointCloud redSphere(const std::shared_ptr<RtPointCloudData>& data)
{
    RtPointCloud cloud;
    cloud.data = data;
    return cloud;
}

}  // namespace

TEST(PointBvhTest, AgreesWithBruteForceOnRandomRays)
{
    const Cloud cloud = randomCloud(4000, 1);
    const PointBvh bvh(cloud.positions.data(), cloud.radii.data(), cloud.radii.size());
    EXPECT_EQ(4000u, bvh.pointCount());
    std::mt19937 rng(2);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    int hits = 0;
    for (const double scale : { 1.0, 2.5, 0.5 }) {
        for (int k = 0; k < 400; ++k) {
            const double o[3] = { u(rng) * 9, u(rng) * 9, u(rng) * 9 };
            double d[3] = { u(rng), u(rng), u(rng) };
            if (k % 7 == 0) d[2] = 0.0;   // axis-aligned components exercise the slab test
            if (d[0] == 0.0 && d[1] == 0.0 && d[2] == 0.0) d[0] = 1.0;
            uint32_t refIndex = 0;
            double refT = 0.0;
            const bool ref = bruteForce(cloud, o, d, 1.0e-4, 1.0e30, scale, refIndex, refT);
            PointBvh::Hit hit;
            const bool got = bvh.closestHit(cloud.positions.data(), cloud.radii.data(), o, d, 1.0e-4, 1.0e30, scale, hit);
            ASSERT_EQ(ref, got) << "ray " << k << " scale " << scale;
            if (ref) {
                ++hits;
                EXPECT_NEAR(refT, hit.t, 1.0e-9 * std::max(1.0, std::abs(refT)));
                EXPECT_EQ(refIndex, hit.index);
            }
        }
    }
    EXPECT_GT(hits, 300);   // the test must actually hit things
}

TEST(PointBvhTest, EmptySingleAndRayStartingInsideASphere)
{
    PointBvh empty(nullptr, nullptr, 0);
    PointBvh::Hit hit;
    const double o[3] = { 0, 0, 0 }, d[3] = { 0, 0, 1 };
    EXPECT_FALSE(empty.closestHit(nullptr, nullptr, o, d, 0.0, 1e9, 1.0, hit));

    const float p[3] = { 0, 0, 5 };
    const float r[1] = { 1.f };
    PointBvh one(p, r, 1);
    ASSERT_TRUE(one.closestHit(p, r, o, d, 1.0e-4, 1e9, 1.0, hit));
    EXPECT_NEAR(4.0, hit.t, 1.0e-9);                       // the near surface
    // From inside the sphere the far surface is the hit.
    const double inside[3] = { 0, 0, 5 };
    ASSERT_TRUE(one.closestHit(p, r, inside, d, 1.0e-4, 1e9, 1.0, hit));
    EXPECT_NEAR(1.0, hit.t, 1.0e-9);
    // A longer direction vector means a shorter t for the same point.
    const double d2[3] = { 0, 0, 2 };
    ASSERT_TRUE(one.closestHit(p, r, o, d2, 1.0e-4, 1e9, 1.0, hit));
    EXPECT_NEAR(2.0, hit.t, 1.0e-9);
    EXPECT_FALSE(one.closestHit(p, r, o, d, 1.0e-4, 3.5, 1.0, hit));   // beyond tMax
    EXPECT_FALSE(one.closestHit(p, r, o, d, 1.0e-4, 1e9, 0.1, hit) && hit.t < 4.5);   // a 0.1 scale shrinks it to radius 0.1
}

TEST(PointBvhTest, DataBuildsItsBvhOnceAndReportsTheTime)
{
    Cloud cloud = randomCloud(20000, 3);
    auto data = cloud.data();
    EXPECT_EQ(0.0, data->bvhBuildMilliseconds());
    const PointBvh* first = &data->bvh();
    EXPECT_GT(data->bvhBuildMilliseconds(), 0.0);
    EXPECT_EQ(first, &data->bvh());                         // built once, shared
    EXPECT_GT(first->nodeCount(), 20000u / 6);
}

TEST(PointCloudRenderTest, ASphereCloudIsSeenInItsOwnColourAndDoesNotNeedTriangles)
{
    Cloud cloud;
    cloud.positions = { 0, 0, 0 };
    cloud.radii = { 1.f };
    cloud.colors = { 1.f, 0.f, 0.f };
    PathTracer tracer(smallSettings());
    Phantom::Graphics::Imageuc image;
    ASSERT_TRUE(tracer.render({}, frontCamera(), image, {}, {}, { redSphere(cloud.data()) }));
    EXPECT_TRUE(isRed(image, 24, 24));                       // the sphere at the centre
    EXPECT_FALSE(isRed(image, 2, 2));                        // the sky in the corner
    EXPECT_FALSE(isRed(image, 45, 24));

    // The same cloud with a uniform colour override and a green tint: the tint multiplies, so red x green = black albedo.
    RtPointCloud tinted = redSphere(cloud.data());
    tinted.tint[0] = 0.0;
    Phantom::Graphics::Imageuc dark;
    ASSERT_TRUE(tracer.render({}, frontCamera(), dark, {}, {}, { tinted }));
    EXPECT_FALSE(isRed(dark, 24, 24));
    RtPointCloud uniform = redSphere(cloud.data());
    uniform.useUniformColor = true;
    uniform.uniformColor[0] = 0.0; uniform.uniformColor[1] = 0.0; uniform.uniformColor[2] = 1.0;
    Phantom::Graphics::Imageuc blue;
    ASSERT_TRUE(tracer.render({}, frontCamera(), blue, {}, {}, { uniform }));
    EXPECT_FALSE(isRed(blue, 24, 24));
    const auto c = blue.getColor(24, 24);
    EXPECT_GT(c.b, c.r);
}

TEST(PointCloudRenderTest, NonUniformScaleMakesAnEllipsoidAndTheModelMatrixPlacesIt)
{
    Cloud cloud;
    cloud.positions = { 0, 0, 0 };
    cloud.radii = { 1.f };
    cloud.colors = { 1.f, 0.f, 0.f };
    PathTracer tracer(smallSettings(64));
    RtPointCloud stretched = redSphere(cloud.data());
    stretched.modelToWorld[0] = 2.0;   // scale x by 2
    Phantom::Graphics::Imageuc image;
    ASSERT_TRUE(tracer.render({}, frontCamera(), image, {}, {}, { stretched }));
    int width = 0, height = 0;
    for (int x = 0; x < 64; ++x) if (isRed(image, x, 32)) ++width;
    for (int y = 0; y < 64; ++y) if (isRed(image, 32, y)) ++height;
    EXPECT_GT(height, 8);
    EXPECT_NEAR(static_cast<double>(width) / height, 2.0, 0.35);     // an ellipsoid twice as wide as it is tall

    // Translation moves it: shifted +1.5 in x (about 18 px at this camera) it leaves the centre and appears to the right.
    RtPointCloud moved = redSphere(cloud.data());
    moved.modelToWorld[12] = 1.5;
    Phantom::Graphics::Imageuc side;
    ASSERT_TRUE(tracer.render({}, frontCamera(), side, {}, {}, { moved }));
    EXPECT_FALSE(isRed(side, 32, 32));
    EXPECT_TRUE(isRed(side, 32 + 18, 32));
}

TEST(PointCloudRenderTest, MeshesAndPointCloudsOccludeEachOther)
{
    Cloud cloud;
    cloud.positions = { 0, 0, 0 };
    cloud.radii = { 1.f };
    cloud.colors = { 1.f, 0.f, 0.f };
    // A grey wall in front of the sphere (z = 3), covering the view.
    RtTriangle a, b;
    const double w = 6.0;
    const double v0[3] = { -w, -w, 3 }, v1[3] = { w, -w, 3 }, v2[3] = { w, w, 3 }, v3[3] = { -w, w, 3 };
    for (int k = 0; k < 3; ++k) { a.v0[k] = v0[k]; a.v1[k] = v1[k]; a.v2[k] = v2[k]; b.v0[k] = v0[k]; b.v1[k] = v2[k]; b.v2[k] = v3[k]; }
    for (int k = 0; k < 3; ++k) { a.albedo[k] = 0.5; b.albedo[k] = 0.5; }
    PathTracer tracer(smallSettings());
    Phantom::Graphics::Imageuc hidden;
    ASSERT_TRUE(tracer.render({ a, b }, frontCamera(), hidden, {}, {}, { redSphere(cloud.data()) }));
    EXPECT_FALSE(isRed(hidden, 24, 24));                     // the wall hides the cloud

    // Behind the wall's plane the other way round: the sphere in front of the wall is visible.
    RtPointCloud front = redSphere(cloud.data());
    front.modelToWorld[14] = 5.0;                            // z = 5, nearer the camera than the wall
    Phantom::Graphics::Imageuc shown;
    ASSERT_TRUE(tracer.render({ a, b }, frontCamera(), shown, {}, {}, { front }));
    EXPECT_TRUE(isRed(shown, 24, 24));
}

TEST(PointCloudRenderTest, PointCloudsCastShadowsOnMeshes)
{
    // One big sphere cloud between a directional light (coming from +y) and a floor: the floor under it is darker.
    Cloud cloud;
    cloud.positions = { 0, 2, 0 };
    cloud.radii = { 1.5f };
    cloud.colors = { 1.f, 1.f, 1.f };
    RtTriangle a, b;
    const double w = 12.0;
    const double f0[3] = { -w, 0, -w }, f1[3] = { w, 0, -w }, f2[3] = { w, 0, w }, f3[3] = { -w, 0, w };
    for (int k = 0; k < 3; ++k) { a.v0[k] = f0[k]; a.v1[k] = f2[k]; a.v2[k] = f1[k]; b.v0[k] = f0[k]; b.v1[k] = f3[k]; b.v2[k] = f2[k]; a.albedo[k] = 0.8; b.albedo[k] = 0.8; }
    RtLight sun;
    sun.type = RtLight::Type::Directional;
    sun.direction[0] = 0; sun.direction[1] = -1; sun.direction[2] = 0;
    sun.intensity = 6.0;
    RtCameraSpec cam;
    cam.lookFrom[0] = 0; cam.lookFrom[1] = 14; cam.lookFrom[2] = 0.01;
    cam.lookAt[0] = 0; cam.lookAt[1] = 0; cam.lookAt[2] = 0;
    cam.up[0] = 0; cam.up[1] = 0; cam.up[2] = -1;
    cam.fovDeg = 40.0;
    RenderSettings s = smallSettings(80);
    s.samplesPerPixel = 64;
    PathTracer tracer(s);
    // Seen from the side, the floor point straight under the sphere is not hidden by it, but the sphere is in the way of the light.
    // At 45 degrees the radius-1.5 sphere itself covers the floor origin (2 / sqrt(2) < 1.5).
    // A lower eye separates the white sphere's silhouette from the patch being measured.
    cam.lookFrom[0] = 0; cam.lookFrom[1] = 8; cam.lookFrom[2] = 25;
    Phantom::Graphics::Imageuc sideWith, sideWithout;
    ASSERT_TRUE(tracer.render({ a, b }, cam, sideWith, {}, { sun }, { redSphere(cloud.data()) }));
    ASSERT_TRUE(tracer.render({ a, b }, cam, sideWithout, {}, { sun }, {}));
    // The floor point straight under the sphere (world 0,0,0) is the image centre.
    // A small floor patch inside the projected shadow, averaged over independent samples.
    auto patch = [](const Phantom::Graphics::Imageuc& image) {
        double sum = 0.0;
        for (int y = 39; y <= 40; ++y) for (int x = 39; x <= 40; ++x) { const auto c = image.getColor(x, y); sum += c.r + c.g + c.b; }
        return sum / 4.0;
    };
    EXPECT_LT(patch(sideWith), patch(sideWithout) - 60.0);
}

TEST(PointCloudRenderTest, EmptyAndMissingCloudsAreSkippedAndAManyPointCloudRenders)
{
    PathTracer tracer(smallSettings(24));
    Phantom::Graphics::Imageuc image;
    RtPointCloud none;                                       // no data at all
    Cloud emptyCloud;
    emptyCloud.radii = {};
    EXPECT_TRUE(tracer.render({}, frontCamera(), image, {}, {}, { none }));

    Cloud big = randomCloud(30000, 5);
    big.colors.assign(big.radii.size() * 3, 0.f);
    for (size_t i = 0; i < big.radii.size(); ++i) big.colors[3 * i] = 1.f;
    EXPECT_TRUE(tracer.render({}, frontCamera(), image, {}, {}, { redSphere(big.data()) }));
}
