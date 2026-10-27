// GPU frame output pass (gpu::FramePresenter, cs_frame_output): sizes and pixels equal the software bridge
// (CoreWrapper::OnFrameComplete through frame_path_cases.hpp's CpuFramePath) for every frame path case, with the
// deinterlace setting off and on, alpha ignored. GPU cases SKIP without a Vulkan device and fail on any validation
// error.
#include "catch_amalgamated.hpp"
#include "frame_path_cases.hpp"
#include "gpu_test_device.hpp"

#include <gpu/frame_presenter.hpp>
#include <gpu/vk_util.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace brimir::gpu;
using brimir::test::CloseAndCheckValidation;
using brimir::test::CpuFramePath;
using brimir::test::FramePathCase;
using brimir::test::MakePatternFrame;
using brimir::test::OpenDevice;

namespace {

constexpr uint32_t kSlots = 3;

OutputSettings SettingsOf(const FramePathCase &c) {
    return OutputSettings{.cropH = c.cropH, .cropV = c.cropV, .rotation = c.rotation};
}

std::vector<FramePathCase> AllCases() {
    std::vector<FramePathCase> cases = brimir::test::FramePathCases();
    const auto &extras = brimir::test::FramePathExtraCases();
    cases.insert(cases.end(), extras.begin(), extras.end());
    return cases;
}

std::string Describe(const FramePathCase &c) {
    return std::to_string(c.w) + "x" + std::to_string(c.h) + " crop " + std::to_string(c.cropH) + "/" +
           std::to_string(c.cropV) + " rot " + std::to_string(c.rotation) + (c.deinterlace ? " deint" : "");
}

// Number of pixels whose RGB differs; reports the first one.
size_t CountMismatches(const std::vector<uint32_t> &gpu, const std::vector<uint32_t> &cpu, uint32_t width) {
    if (gpu.size() != cpu.size()) {
        UNSCOPED_INFO("size " << gpu.size() << " vs " << cpu.size());
        return cpu.size() + 1;
    }
    size_t mismatches = 0;
    for (size_t i = 0; i < cpu.size(); ++i) {
        if ((gpu[i] & 0x00FFFFFFu) != (cpu[i] & 0x00FFFFFFu)) {
            if (mismatches == 0) {
                UNSCOPED_INFO("first mismatch at (" << i % width << ", " << i / width << "): gpu 0x" << std::hex
                                                    << gpu[i] << " cpu 0x" << cpu[i] << std::dec);
            }
            ++mismatches;
        }
    }
    return mismatches;
}

// The bridge's w x h frame as the output image holds it (gpu::WrittenSize): plus a border column and row repeating
// the last column and row, where the image has room for them.
std::vector<uint32_t> WithBorder(const std::vector<uint32_t> &frame, uint32_t w, uint32_t h) {
    const OutputSize written = WrittenSize({w, h});
    std::vector<uint32_t> out(static_cast<size_t>(written.width) * written.height);
    for (uint32_t y = 0; y < written.height; ++y) {
        for (uint32_t x = 0; x < written.width; ++x) {
            out[static_cast<size_t>(y) * written.width + x] =
                frame[static_cast<size_t>(std::min(y, h - 1)) * w + std::min(x, w - 1)];
        }
    }
    return out;
}

bool PresentBatch(VulkanDevice &device, FramePresenter &presenter, const std::vector<const FramePathCase *> &batch,
                  const std::vector<std::vector<uint32_t>> &frames) {
    bool presented = true;
    const VkResult result = vk_util::SubmitOneShot(device, [&](VkCommandBuffer cmd) {
        for (uint32_t slot = 0; slot < batch.size(); ++slot) {
            const FramePathCase &c = *batch[slot];
            presented = presenter.Present(slot, frames[slot].data(), c.w, c.h, SettingsOf(c), cmd) && presented;
        }
    });
    return presented && result == VK_SUCCESS;
}

} // namespace

TEST_CASE("ComputeOutputSize equals the bridge's output size for every frame path case", "[gpu][frame-output]") {
    for (const auto &c : AllCases()) {
        uint32_t w = 0;
        uint32_t h = 0;
        CpuFramePath(c, &w, &h);
        INFO(Describe(c));
        CHECK(ComputeOutputSize(c.w, c.h, SettingsOf(c)) == OutputSize{w, h});
    }
    // Rotations the bridge does not know are treated as 0.
    CHECK(ComputeOutputSize(320, 224, {.rotation = 45}) == OutputSize{320, 224});
    // Crops above INT_MAX (the bridge's crops are int) never apply, including values where crop + 32 would wrap
    // around in 32 bits.
    CHECK(ComputeOutputSize(320, 224, {.cropH = 0xFFFFFFF0u, .cropV = 0}) == OutputSize{320, 224});
    CHECK(ComputeOutputSize(320, 224, {.cropH = 0, .cropV = 0x80000000u}) == OutputSize{320, 224});
    // A crop at the guard's limit is rejected, one below it applies (and both agree with the bridge).
    CHECK(ComputeOutputSize(320, 224, {.cropH = 288, .cropV = 0}) == OutputSize{320, 224});
    CHECK(ComputeOutputSize(320, 224, {.cropH = 287, .cropV = 0}) == OutputSize{33, 224});
    for (const FramePathCase c : {FramePathCase{320, 224, 288, 0, 0, false}, FramePathCase{320, 224, 287, 0, 0, false},
                                  FramePathCase{320, 224, 0, 191, 90, false}, FramePathCase{320, 224, 0, 192, 90, false}}) {
        uint32_t w = 0;
        uint32_t h = 0;
        CpuFramePath(c, &w, &h);
        INFO(Describe(c));
        CHECK(ComputeOutputSize(c.w, c.h, SettingsOf(c)) == OutputSize{w, h});
    }
}

namespace {

// A distinct pixel per N x source pixel (XBGR), so a wrong sub-pixel shows up.
std::vector<uint32_t> MakeScaledPattern(uint32_t w, uint32_t h, uint32_t n) {
    return MakePatternFrame(w * n, h * n);
}

// CPU reference of the frame output at internal resolution n: the N x source (pitch n * w) of a w x h native frame,
// with the crop decided on the native size (ComputeOutputSize at scale 1), the crop offsets n * (crop / 2), then the
// bridge's rotations on the N x frame; XBGR -> XRGB. At n = 1 it is the bridge (checked below).
std::vector<uint32_t> ScaledFramePath(const std::vector<uint32_t> &source, const FramePathCase &c, uint32_t n,
                                      OutputSize &out) {
    const OutputSize cropped = ComputeOutputSize(c.w, c.h, {.cropH = c.cropH, .cropV = c.cropV});
    const bool crop = cropped != OutputSize{c.w, c.h};
    const uint32_t left = crop ? n * (c.cropH / 2) : 0;
    const uint32_t top = crop ? n * (c.cropV / 2) : 0;
    const uint32_t cw = n * cropped.width;
    const uint32_t ch = n * cropped.height;
    const uint32_t pitch = n * c.w;
    const bool swap = c.rotation == 90 || c.rotation == 270;
    out = swap ? OutputSize{ch, cw} : OutputSize{cw, ch};
    std::vector<uint32_t> frame(static_cast<size_t>(cw) * ch);
    // The bridge's loops (CoreWrapper::OnFrameComplete): destination from source.
    for (uint32_t sy = 0; sy < ch; ++sy) {
        for (uint32_t sx = 0; sx < cw; ++sx) {
            const uint32_t xbgr = source[static_cast<size_t>(sy + top) * pitch + sx + left];
            const uint32_t xrgb = ((xbgr & 0xFFu) << 16) | (xbgr & 0xFF00u) | ((xbgr >> 16) & 0xFFu);
            uint32_t dx = sx;
            uint32_t dy = sy;
            if (c.rotation == 90) {
                dx = ch - 1 - sy;
                dy = sx;
            } else if (c.rotation == 180) {
                dx = cw - 1 - sx;
                dy = ch - 1 - sy;
            } else if (c.rotation == 270) {
                dx = sy;
                dy = cw - 1 - sx;
            }
            frame[static_cast<size_t>(dy) * out.width + dx] = xrgb;
        }
    }
    return frame;
}

// As WithBorder, with the border clamped to an image of `maxSize`.
std::vector<uint32_t> WithBorderIn(const std::vector<uint32_t> &frame, uint32_t w, uint32_t h, uint32_t maxSize) {
    const OutputSize written = WrittenSize({w, h}, maxSize);
    std::vector<uint32_t> out(static_cast<size_t>(written.width) * written.height);
    for (uint32_t y = 0; y < written.height; ++y) {
        for (uint32_t x = 0; x < written.width; ++x) {
            out[static_cast<size_t>(y) * written.width + x] =
                frame[static_cast<size_t>(std::min(y, h - 1)) * w + std::min(x, w - 1)];
        }
    }
    return out;
}

std::vector<FramePathCase> ScaledCases() {
    std::vector<FramePathCase> cases;
    for (uint32_t w : {320u, 704u}) {
        for (uint32_t h : {224u, 512u}) {
            for (const auto &crop : {std::pair{0u, 0u}, std::pair{16u, 16u}, std::pair{21u, 9u}}) {
                for (uint32_t rotation : {0u, 90u, 180u, 270u}) {
                    cases.push_back({w, h, crop.first, crop.second, rotation, false});
                }
            }
        }
    }
    // The crop guard decides on the native size: at N x the frame would exceed these crops by more than 32 px.
    for (uint32_t rotation : {0u, 90u}) {
        cases.push_back({320, 224, 300, 0, rotation, false});
        cases.push_back({320, 224, 0, 200, rotation, false});
    }
    return cases;
}

} // namespace

TEST_CASE("ComputeOutputSize at internal resolution N is N x the native output size", "[gpu][frame-output]") {
    for (uint32_t n = 1; n <= FramePresenter::kMaxScale; ++n) {
        for (const auto &c : AllCases()) {
            INFO(Describe(c) << " at " << n << "x");
            OutputSettings settings = SettingsOf(c);
            const OutputSize native = ComputeOutputSize(c.w, c.h, settings);
            settings.scale = n;
            CHECK(ComputeOutputSize(c.w, c.h, settings) == OutputSize{n * native.width, n * native.height});
        }
    }
    // The guard is native: 640 > 576 + 32 at 2x, but the native 320 is not > 288 + 32.
    CHECK(ComputeOutputSize(320, 224, {.cropH = 288, .cropV = 0, .scale = 2}) == OutputSize{640, 448});
    CHECK(ComputeOutputSize(320, 224, {.cropH = 287, .cropV = 0, .scale = 2}) == OutputSize{66, 448});
    // Scale 0 is treated as 1.
    CHECK(ComputeOutputSize(320, 224, {.scale = 0}) == OutputSize{320, 224});
    // The border clamps to the image (704 N).
    CHECK(WrittenSize({2816, 2048}, 2816) == OutputSize{2816, 2049});
    CHECK(WrittenSize({2048, 2816}, 2816) == OutputSize{2049, 2816});
}

TEST_CASE("The N x reference of the frame output is the bridge at 1x", "[gpu][frame-output]") {
    for (const auto &c : AllCases()) {
        INFO(Describe(c));
        uint32_t w = 0;
        uint32_t h = 0;
        const std::vector<uint32_t> bridge = CpuFramePath(c, &w, &h);
        OutputSize size;
        const std::vector<uint32_t> reference = ScaledFramePath(MakePatternFrame(c.w, c.h), c, 1, size);
        REQUIRE(size == OutputSize{w, h});
        CHECK(CountMismatches(reference, bridge, w) == 0);
    }
}

TEST_CASE("Frame output pass at internal resolution N equals the N x reference", "[gpu][frame-output]") {
    const uint32_t n = GENERATE(2u, 3u, 4u);
    CAPTURE(n);
    auto device = OpenDevice();
    {
        FramePresenter presenter(*device, kSlots, n);
        REQUIRE(presenter.Ok());
        REQUIRE(presenter.Scale() == n);
        REQUIRE(presenter.MaxOutputSize() == 704 * n);
        vk_util::Buffer source;
        REQUIRE(vk_util::CreateBuffer(*device, VkDeviceSize{704 * n} * (512 * n) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                      source) == VK_SUCCESS);
        REQUIRE(source.mapped != nullptr);
        size_t failures = 0;
        uint32_t slot = 0;
        for (const FramePathCase &c : ScaledCases()) {
            INFO(Describe(c) << " in slot " << slot);
            const std::vector<uint32_t> frame = MakeScaledPattern(c.w, c.h, n);
            std::memcpy(source.mapped, frame.data(), frame.size() * sizeof(uint32_t));
            OutputSettings settings = SettingsOf(c);
            settings.scale = n;
            bool presented = false;
            REQUIRE(vk_util::SubmitOneShot(*device, [&](VkCommandBuffer cmd) {
                        presented = presenter.PresentDevice(slot, source.buffer, c.w, c.h, settings, cmd);
                    }) == VK_SUCCESS);
            REQUIRE(presented);
            OutputSize size;
            const std::vector<uint32_t> expected = ScaledFramePath(frame, c, n, size);
            REQUIRE(ComputeOutputSize(c.w, c.h, settings) == size);
            const OutputSize written = WrittenSize(size, presenter.MaxOutputSize());
            const size_t mismatches =
                CountMismatches(presenter.ReadbackXRGB(slot, written),
                                WithBorderIn(expected, size.width, size.height, presenter.MaxOutputSize()),
                                written.width);
            CHECK(mismatches == 0);
            failures += mismatches != 0;
            slot = (slot + 1) % kSlots;
        }
        CHECK(failures == 0);
        // More than the presenter's scale, and an upload (native only) at N x, are refused.
        const std::vector<uint32_t> native = MakePatternFrame(320, 224);
        CHECK(vk_util::SubmitOneShot(*device, [&](VkCommandBuffer cmd) {
                  CHECK_FALSE(presenter.PresentDevice(0, source.buffer, 320, 224, {.scale = n + 1}, cmd));
                  CHECK_FALSE(presenter.Present(0, native.data(), 320, 224, {.scale = n}, cmd));
              }) == VK_SUCCESS);
        vk_util::DestroyBuffer(*device, source);
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("Frame output pass equals the software bridge for every frame path case", "[gpu][frame-output]") {
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);

    // Each case with deinterlace off and on (the pass ignores it; the output must not change).
    std::vector<FramePathCase> cases;
    for (FramePathCase c : AllCases()) {
        c.deinterlace = false;
        cases.push_back(c);
        c.deinterlace = true;
        cases.push_back(c);
    }

    {
        FramePresenter presenter(*device, kSlots);
        REQUIRE(presenter.Ok());
        REQUIRE(presenter.SlotCount() == kSlots);

        // Fill every slot in one command buffer, then read all of them back: a slot mix-up in the uploads, the
        // descriptors or the images shows up as another case's pixels. Successive batches start at different slots.
        size_t failures = 0;
        for (size_t first = 0; first < cases.size(); first += kSlots) {
            std::vector<const FramePathCase *> batch(kSlots);
            std::vector<std::vector<uint32_t>> frames(kSlots);
            const size_t rotate = (first / kSlots) % kSlots;
            for (uint32_t slot = 0; slot < kSlots; ++slot) {
                batch[slot] = &cases[(first + (slot + rotate) % kSlots) % cases.size()];
                frames[slot] = MakePatternFrame(batch[slot]->w, batch[slot]->h);
            }
            REQUIRE(PresentBatch(*device, presenter, batch, frames));

            for (uint32_t slot = 0; slot < kSlots; ++slot) {
                const FramePathCase &c = *batch[slot];
                uint32_t w = 0;
                uint32_t h = 0;
                const std::vector<uint32_t> expected = CpuFramePath(c, &w, &h);
                const OutputSize size = ComputeOutputSize(c.w, c.h, SettingsOf(c));
                // The frame and its replicated border.
                const OutputSize written = WrittenSize(size);
                const std::vector<uint32_t> actual = presenter.ReadbackXRGB(slot, written);
                INFO(Describe(c) << " in slot " << slot);
                REQUIRE(size == OutputSize{w, h});
                CHECK(written == OutputSize{std::min(w + 1, FramePresenter::kMaxOutputSize),
                                            std::min(h + 1, FramePresenter::kMaxOutputSize)});
                const size_t mismatches = CountMismatches(actual, WithBorder(expected, w, h), written.width);
                CHECK(mismatches == 0);
                failures += mismatches != 0;
            }
        }
        CHECK(failures == 0);
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("Frame output slots keep their own frames", "[gpu][frame-output]") {
    auto device = OpenDevice();
    {
        FramePresenter presenter(*device, kSlots);
        REQUIRE(presenter.Ok());
        for (uint32_t slot = 0; slot < kSlots; ++slot) {
            CHECK(presenter.OutputImage(slot) != VK_NULL_HANDLE);
            CHECK(presenter.OutputView(slot) != VK_NULL_HANDLE);
        }
        CHECK(presenter.OutputImage(kSlots) == VK_NULL_HANDLE);
        CHECK(presenter.OutputView(kSlots) == VK_NULL_HANDLE);

        // Same case, a different pattern per slot: XOR with a salt whose R and B bytes are equal, so the salt
        // survives the XBGR -> XRGB swap unchanged and the expected output is the bridge's XOR the salt.
        const FramePathCase c{352, 240, 16, 16, 90, false};
        const OutputSettings settings = SettingsOf(c);
        const OutputSize size = ComputeOutputSize(c.w, c.h, settings);
        const std::vector<uint32_t> base = CpuFramePath(c);
        auto salt = [](uint32_t slot, uint32_t round) { return ((slot + 1) * 0x00250025u) ^ (round * 0x00004000u); };
        auto expectedFor = [&](uint32_t s) {
            std::vector<uint32_t> out(base);
            for (auto &px : out) {
                px ^= s;
            }
            return out;
        };
        auto present = [&](uint32_t slot, uint32_t round) {
            std::vector<uint32_t> frame = MakePatternFrame(c.w, c.h);
            for (auto &px : frame) {
                px ^= salt(slot, round);
            }
            bool presented = false;
            const VkResult result = vk_util::SubmitOneShot(*device, [&](VkCommandBuffer cmd) {
                presented = presenter.Present(slot, frame.data(), c.w, c.h, settings, cmd);
            });
            return presented && result == VK_SUCCESS;
        };

        for (uint32_t slot = 0; slot < kSlots; ++slot) {
            REQUIRE(present(slot, 0));
        }
        // Overwrite only the middle slot; the others keep round 0.
        REQUIRE(present(1, 1));
        for (uint32_t slot = 0; slot < kSlots; ++slot) {
            INFO("slot " << slot);
            const uint32_t round = slot == 1 ? 1 : 0;
            CHECK(CountMismatches(presenter.ReadbackXRGB(slot, WrittenSize(size)),
                                  WithBorder(expectedFor(salt(slot, round)), size.width, size.height),
                                  WrittenSize(size).width) == 0);
        }
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("FramePresenter::Present rejects invalid input without recording", "[gpu][frame-output]") {
    auto device = OpenDevice();
    {
        FramePresenter presenter(*device, 2);
        REQUIRE(presenter.Ok());
        const std::vector<uint32_t> frame = MakePatternFrame(FramePresenter::kMaxSourceWidth + 1,
                                                             FramePresenter::kMaxSourceHeight + 1);
        const VkResult result = vk_util::SubmitOneShot(*device, [&](VkCommandBuffer cmd) {
            CHECK_FALSE(presenter.Present(2, frame.data(), 320, 224, {}, cmd));
            CHECK_FALSE(presenter.Present(0, nullptr, 320, 224, {}, cmd));
            CHECK_FALSE(presenter.Present(0, frame.data(), 0, 224, {}, cmd));
            CHECK_FALSE(presenter.Present(0, frame.data(), 320, 0, {}, cmd));
            CHECK_FALSE(presenter.Present(0, frame.data(), FramePresenter::kMaxSourceWidth + 1, 224, {}, cmd));
            CHECK_FALSE(presenter.Present(0, frame.data(), 320, FramePresenter::kMaxSourceHeight + 1, {}, cmd));
        });
        CHECK(result == VK_SUCCESS);
        // The largest frame is accepted.
        CHECK(vk_util::SubmitOneShot(*device, [&](VkCommandBuffer cmd) {
                  CHECK(presenter.Present(1, frame.data(), FramePresenter::kMaxSourceWidth,
                                          FramePresenter::kMaxSourceHeight, {.rotation = 270}, cmd));
              }) == VK_SUCCESS);
        CHECK(presenter.ReadbackXRGB(1, {512, 704}).size() == 512u * 704u);
        // Its border: a column only (the image is 704 high), and the pixels with it equal the bridge's.
        {
            const FramePathCase c{FramePresenter::kMaxSourceWidth, FramePresenter::kMaxSourceHeight, 0, 0, 270, false};
            const std::vector<uint32_t> largest = MakePatternFrame(c.w, c.h);
            REQUIRE(vk_util::SubmitOneShot(*device, [&](VkCommandBuffer cmd) {
                        CHECK(presenter.Present(0, largest.data(), c.w, c.h, SettingsOf(c), cmd));
                    }) == VK_SUCCESS);
            uint32_t w = 0;
            uint32_t h = 0;
            const std::vector<uint32_t> expected = CpuFramePath(c, &w, &h);
            REQUIRE(OutputSize{w, h} == OutputSize{512, 704});
            REQUIRE(WrittenSize({w, h}) == OutputSize{513, 704});
            CHECK(CountMismatches(presenter.ReadbackXRGB(0, {513, 704}), WithBorder(expected, w, h), 513) == 0);
        }
        CHECK(presenter.ReadbackXRGB(1, {705, 1}).empty());
        CHECK(presenter.ReadbackXRGB(2, {1, 1}).empty());
    }
    CloseAndCheckValidation(device);
}
