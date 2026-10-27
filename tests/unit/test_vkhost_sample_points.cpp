// brimir_vkhost's sample points (--hash-sample) and sharpness metric (--sharpness), tools/vkhost/sample_points.hpp:
// on synthetic frames, a nearest upscale has every block constant and its sample points are the 1x image (in every
// rotation); added sub-pixel detail gives exactly the expected non-constant fraction and mean absolute difference.
#include "catch_amalgamated.hpp"

#include "../../tools/vkhost/sample_points.hpp"

#include <cstdint>
#include <vector>

using vkhost::ComputeSharpness;
using vkhost::Region;
using vkhost::SampleImage;

namespace {

// A native output image with a distinct colour per pixel (XRGB).
std::vector<uint32_t> Pattern(uint32_t w, uint32_t h) {
    std::vector<uint32_t> img(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < img.size(); ++i) {
        img[i] = static_cast<uint32_t>((i * 0x9E3779u + 0x123457u) & 0xFFFFFFu);
    }
    return img;
}

// Nearest upscale by n in output space.
std::vector<uint32_t> Nearest(const std::vector<uint32_t> &img, uint32_t w, uint32_t h, uint32_t n) {
    std::vector<uint32_t> out(static_cast<size_t>(w) * n * h * n);
    for (uint32_t Y = 0; Y < h * n; ++Y) {
        for (uint32_t X = 0; X < w * n; ++X) {
            out[static_cast<size_t>(Y) * w * n + X] = img[static_cast<size_t>(Y / n) * w + X / n];
        }
    }
    return out;
}

} // namespace

TEST_CASE("vkhost sharpness: a nearest upscale is constant and samples to the 1x image", "[vkhost][scale]") {
    constexpr uint32_t w = 20, h = 12; // low-res progressive sizes exclude nothing
    const std::vector<uint32_t> img = Pattern(w, h);
    for (uint32_t n = 1; n <= 4; ++n) {
        for (uint32_t rotation : {0u, 90u, 180u, 270u}) {
            CAPTURE(n, rotation);
            const std::vector<uint32_t> frame = Nearest(img, w, h, n);
            const vkhost::Sharpness s = ComputeSharpness(frame.data(), w * n, h * n, n, rotation);
            REQUIRE(s.valid);
            CHECK(s.blocks == uint64_t{w} * h);
            CHECK(s.nonConstant == 0);
            CHECK(s.nonConstantFraction == 0.0);
            CHECK(s.meanAbsDiff == 0.0);
            CHECK(SampleImage(frame.data(), w * n, h * n, n, rotation) == img);
        }
    }
}

TEST_CASE("vkhost sharpness: sub-pixel detail gives the expected fraction and difference", "[vkhost][scale]") {
    constexpr uint32_t w = 20, h = 12, n = 2;
    const std::vector<uint32_t> img = Pattern(w, h);
    std::vector<uint32_t> frame = Nearest(img, w, h, n);
    // Every 4th block (native index % 4 == 0) gets detail in its bottom-right sub-pixel (never the sample point):
    // red +10 or -10 (whichever stays in range).
    uint64_t changed = 0;
    for (uint32_t oy = 0; oy < h; ++oy) {
        for (uint32_t ox = 0; ox < w; ++ox) {
            if ((oy * w + ox) % 4 != 0) {
                continue;
            }
            uint32_t &px = frame[static_cast<size_t>(oy * n + 1) * w * n + ox * n + 1];
            const uint32_t red = (px >> 16) & 0xFF;
            px = (px & 0x00FFFFu) | ((red < 128 ? red + 10 : red - 10) << 16);
            ++changed;
        }
    }
    const vkhost::Sharpness s = ComputeSharpness(frame.data(), w * n, h * n, n, 0);
    REQUIRE(s.valid);
    CHECK(s.blocks == uint64_t{w} * h);
    CHECK(s.nonConstant == changed);
    CHECK(s.nonConstantFraction == Catch::Approx(0.25));
    CHECK(s.meanAbsDiff == Catch::Approx(10.0 * static_cast<double>(changed) / (w * h * n * n * 3.0)));
    // The sample points are untouched: still the 1x image.
    CHECK(SampleImage(frame.data(), w * n, h * n, n, 0) == img);
    // A region: the first row only (blocks 0, 4, 8, 12, 16 changed of 20), clamped at the frame's edge.
    const vkhost::Sharpness row = ComputeSharpness(frame.data(), w * n, h * n, n, 0, Region{0, 0, 1000, 1});
    CHECK(row.w == w);
    CHECK(row.h == 1);
    CHECK(row.blocks == w);
    CHECK(row.nonConstant == 5);
}

TEST_CASE("vkhost sample points: the excluded lattice and sizes that are not a multiple", "[vkhost][scale]") {
    // Hi-res (pre-rotation width 640 > 400): odd native columns are 0.
    constexpr uint32_t w = 640, h = 4, n = 2;
    const std::vector<uint32_t> img = Pattern(w, h);
    const std::vector<uint32_t> frame = Nearest(img, w, h, n);
    const std::vector<uint32_t> samples = SampleImage(frame.data(), w * n, h * n, n, 0);
    REQUIRE(samples.size() == img.size());
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            CHECK(samples[i] == ((x & 1) != 0 ? 0u : img[i]));
        }
    }
    // Interlaced (height above 300): odd native rows.
    const std::vector<uint32_t> tall = Pattern(4, 448);
    const std::vector<uint32_t> tallFrame = Nearest(tall, 4, 448, 1);
    const std::vector<uint32_t> tallSamples = SampleImage(tallFrame.data(), 4, 448, 1, 0);
    CHECK(tallSamples[4] == 0u);
    CHECK(tallSamples[8] == tall[8]);
    CHECK(SampleImage(frame.data(), w * n - 1, h * n, n, 0).empty());
    CHECK_FALSE(ComputeSharpness(frame.data(), w * n, h * n - 1, n, 0).valid);
}
