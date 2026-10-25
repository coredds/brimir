// GPU frame output pass (gpu::FramePresenter, cs_frame_output): sizes and pixels equal the software bridge
// (CoreWrapper::OnFrameComplete through frame_path_cases.hpp's CpuFramePath) for every frame path case, with the
// deinterlace setting off and on, alpha ignored. GPU cases SKIP without a Vulkan device and fail on any validation
// error.
#include "catch_amalgamated.hpp"
#include "frame_path_cases.hpp"
#include "gpu_test_device.hpp"

#include <gpu/frame_presenter.hpp>
#include <gpu/vk_util.hpp>

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
                const std::vector<uint32_t> actual = presenter.ReadbackXRGB(slot, size);
                INFO(Describe(c) << " in slot " << slot);
                REQUIRE(size == OutputSize{w, h});
                const size_t mismatches = CountMismatches(actual, expected, w);
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
            CHECK(CountMismatches(presenter.ReadbackXRGB(slot, size), expectedFor(salt(slot, round)), size.width) ==
                  0);
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
        CHECK(presenter.ReadbackXRGB(1, {705, 1}).empty());
        CHECK(presenter.ReadbackXRGB(2, {1, 1}).empty());
    }
    CloseAndCheckValidation(device);
}
