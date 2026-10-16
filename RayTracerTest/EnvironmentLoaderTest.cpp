#include "pch.h"
#include "../Core/EnvironmentLoader.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
using namespace Phantom::RayTracer;

std::filesystem::path tempFile(const std::string& name) {
    return std::filesystem::temp_directory_path() / name;
}

// A flat Radiance panorama of `w` x `h` texels; every texel decodes to grey (0.5 * 2^(e - 128) * 2) = 2^(e-128) with mantissa 0x80.
std::filesystem::path writeHdr(const std::string& name, int w, int h, unsigned char exponent = 0x81) {
    const auto path = tempFile(name);
    std::ofstream out(path, std::ios::binary);
    out << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " << h << " +X " << w << "\n";
    const char texel[4] = { static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(exponent) };
    for (int i = 0; i < w * h; ++i) out.write(texel, 4);
    return path;
}
}

TEST(EnvironmentLoader, EmptyMissingAndInvalidFilesGiveNull) {
    EXPECT_EQ(nullptr, loadEnvironment("", 1.f, true));
    EXPECT_EQ(nullptr, loadEnvironment(tempFile("phantom_envloader_missing.hdr").string(), 1.f, true));
    const auto bad = tempFile("phantom_envloader_bad.hdr");
    { std::ofstream out(bad, std::ios::binary); out << "not a radiance file"; }
    EXPECT_EQ(nullptr, loadEnvironment(bad.string(), 1.f, true));
    std::filesystem::remove(bad);
}

TEST(EnvironmentLoader, SmallPanoramaIsKeptAtItsSize) {
    const auto hdr = writeHdr("phantom_envloader_small.hdr", 4, 2);
    const auto env = loadEnvironment(hdr.string(), 2.5f, false);
    ASSERT_NE(nullptr, env);
    EXPECT_TRUE(env->valid());
    EXPECT_EQ(4, env->width);
    EXPECT_EQ(2, env->height);
    EXPECT_NEAR(1.0f, env->rgb[0], 0.02f);
    std::filesystem::remove(hdr);
}

TEST(EnvironmentLoader, IntensityAndBackgroundAreIndependentOfTheCachedImage) {
    const auto hdr = writeHdr("phantom_envloader_cache.hdr", 4, 2);
    const auto a = loadEnvironment(hdr.string(), 2.5f, false);
    const auto b = loadEnvironment(hdr.string(), 1.f, true);
    ASSERT_NE(nullptr, a);
    ASSERT_NE(nullptr, b);
    EXPECT_EQ(2.5, a->intensity);
    EXPECT_FALSE(a->background);
    EXPECT_EQ(1.0, b->intensity);
    EXPECT_TRUE(b->background);
    EXPECT_EQ(a->rgb, b->rgb);
    std::filesystem::remove(hdr);
}

TEST(EnvironmentLoader, WidePanoramaIsBoxFilteredToTheWidthLimit) {
    const auto hdr = writeHdr("phantom_envloader_wide.hdr", 2048, 8);
    const auto env = loadEnvironment(hdr.string(), 1.f, true);
    ASSERT_NE(nullptr, env);
    EXPECT_EQ(1024, env->width);
    EXPECT_EQ(4, env->height);
    EXPECT_NEAR(1.0f, env->rgb[0], 0.02f);
    std::filesystem::remove(hdr);
}

TEST(EnvironmentLoader, ChangedFileAtTheSamePathIsReloaded) {
    const auto hdr = writeHdr("phantom_envloader_update.hdr", 4, 2, 0x81);   // grey 1.0
    const auto first = loadEnvironment(hdr.string(), 1.f, true);
    ASSERT_NE(nullptr, first);
    EXPECT_NEAR(1.0f, first->rgb[0], 0.02f);
    writeHdr("phantom_envloader_update.hdr", 4, 2, 0x82);                        // grey 2.0
    std::filesystem::last_write_time(hdr, std::filesystem::last_write_time(hdr) + std::chrono::seconds(5));
    const auto second = loadEnvironment(hdr.string(), 1.f, true);
    ASSERT_NE(nullptr, second);
    EXPECT_NEAR(2.0f, second->rgb[0], 0.04f);
    std::filesystem::remove(hdr);
}
