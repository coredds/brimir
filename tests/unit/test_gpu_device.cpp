// brimir-gpu device layer: the headless device, the owning and non-owning VulkanDevice and the vk_util compute helpers.
// Every case runs on a headless device with the Khronos validation layer when it is installed, fails on any
// validation error (including leaks reported at device destruction) and SKIPs when there is no usable Vulkan device
// (fails instead with BRIMIR_REQUIRE_VULKAN=1).
#include "catch_amalgamated.hpp"
#include "gpu_test_device.hpp"

#include <gpu/gpu_version.hpp>
#include <gpu/headless_device.hpp>
#include <gpu/shaders/generated/cs_fill_test.spv.h>
#include <gpu/vk_device.hpp>
#include <gpu/vk_util.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace brimir::gpu;
namespace vu = brimir::gpu::vk_util;

namespace {

using brimir::test::CloseAndCheckValidation;
using brimir::test::OpenDevice;

// Runs cs_fill_test over `count` uint elements on `device` and returns the buffer contents.
std::vector<uint32_t> RunFill(VulkanDevice &device, uint32_t count) {
    const auto &vk = device.Table();
    std::vector<uint32_t> result;

    vu::Buffer buffer;
    REQUIRE(vu::CreateBuffer(device, count * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             buffer) == VK_SUCCESS);
    REQUIRE(buffer.mapped != nullptr);
    std::memset(buffer.mapped, 0, count * sizeof(uint32_t));

    const VkDescriptorSetLayoutBinding binding{16, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                               nullptr};
    const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    bool ok = vu::CreateDescriptorSetLayout(device, {&binding, 1}, setLayout) == VK_SUCCESS;
    ok = ok && vu::CreateDescriptorPool(device, {&poolSize, 1}, 1, pool) == VK_SUCCESS;
    ok = ok && vu::AllocateDescriptorSet(device, pool, setLayout, set) == VK_SUCCESS;
    ok = ok && vu::CreatePipelineLayout(device, {&setLayout, 1}, {}, pipelineLayout) == VK_SUCCESS;
    ok = ok && vu::CreateComputePipeline(device, shaders::kCsFillTestSpv, pipelineLayout, pipeline) == VK_SUCCESS;
    if (ok) {
        vu::WriteStorageBuffer(device, set, 16, buffer);
        ok = vu::SubmitOneShot(device, [&](VkCommandBuffer cmd) {
                 vk.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                 vk.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &set, 0,
                                            nullptr);
                 vk.vkCmdDispatch(cmd, (count + 63) / 64, 1, 1);
                 vu::GlobalBarrier(device, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
             }) == VK_SUCCESS;
    }
    if (ok) {
        const auto *words = static_cast<const uint32_t *>(buffer.mapped);
        result.assign(words, words + count);
    }

    vk.vkDestroyPipeline(device.Device(), pipeline, nullptr);
    vk.vkDestroyPipelineLayout(device.Device(), pipelineLayout, nullptr);
    vk.vkDestroyDescriptorPool(device.Device(), pool, nullptr);
    vk.vkDestroyDescriptorSetLayout(device.Device(), setLayout, nullptr);
    vu::DestroyBuffer(device, buffer);
    REQUIRE(ok);
    return result;
}

void CheckFill(const std::vector<uint32_t> &values) {
    for (uint32_t i = 0; i < values.size(); ++i) {
        if (values[i] != i * 3 + 1) {
            FAIL("element " << i << " is " << values[i] << ", expected " << i * 3 + 1);
        }
    }
}

// Forwarding stand-ins for a frontend's get_instance_proc_addr / get_device_proc_addr that count their calls.
PFN_vkGetInstanceProcAddr g_realGipa = nullptr;
PFN_vkGetDeviceProcAddr g_realGdpa = nullptr;
std::atomic<uint32_t> g_gipaInstanceCalls{0};
std::atomic<uint32_t> g_gdpaCalls{0};

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CountingGipa(VkInstance instance, const char *name) {
    if (instance != VK_NULL_HANDLE) {
        g_gipaInstanceCalls.fetch_add(1);
    }
    return g_realGipa(instance, name);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL CountingGdpa(VkDevice device, const char *name) {
    g_gdpaCalls.fetch_add(1);
    return g_realGdpa(device, name);
}

ExternalDevice HandlesOf(const VulkanDevice &owner) {
    return ExternalDevice{
        .instance = owner.Instance(),
        .physical = owner.Physical(),
        .device = owner.Device(),
        .queue = owner.Queue(),
        .queueFamily = owner.QueueFamily(),
        .getInstanceProcAddr = volk::vkGetInstanceProcAddr,
        .getDeviceProcAddr = volk::vkGetDeviceProcAddr,
        .apiVersion = kRequiredApiVersion,
    };
}

} // namespace

TEST_CASE("Headless Vulkan device meets the 1.2 compute baseline", "[gpu]") {
    auto device = OpenDevice();
    INFO("device: " << device->Properties().deviceName);

    CHECK(device->OwnsDevice());
    CHECK(device->Instance() != VK_NULL_HANDLE);
    CHECK(device->Physical() != VK_NULL_HANDLE);
    CHECK(device->Device() != VK_NULL_HANDLE);
    CHECK(device->Queue() != VK_NULL_HANDLE);
    CHECK(device->Properties().apiVersion >= kRequiredApiVersion);
    CHECK(device->Features().apiVersion >= kRequiredApiVersion);

    uint32_t familyCount = 0;
    volk::vkGetPhysicalDeviceQueueFamilyProperties(device->Physical(), &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    volk::vkGetPhysicalDeviceQueueFamilyProperties(device->Physical(), &familyCount, families.data());
    REQUIRE(device->QueueFamily() < familyCount);
    CHECK((families[device->QueueFamily()].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0);

    CloseAndCheckValidation(device);
}

TEST_CASE("cs_fill_test writes element i = i * 3 + 1", "[gpu]") {
    auto device = OpenDevice();
    const auto values = RunFill(*device, 1000);
    REQUIRE(values.size() == 1000);
    CheckFill(values);
    CloseAndCheckValidation(device);
}

TEST_CASE("Image helpers create, transition, clear and read back an image", "[gpu]") {
    auto device = OpenDevice();
    const auto &vk = device->Table();
    constexpr uint32_t kWidth = 16;
    constexpr uint32_t kHeight = 8;

    vu::Image image;
    vu::Buffer readback;
    const vu::ImageDesc desc{
        .width = kWidth,
        .height = kHeight,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
    };
    bool ok = vu::CreateImage2D(*device, desc, image) == VK_SUCCESS;
    CHECK(ok);
    CHECK(image.view != VK_NULL_HANDLE);
    ok = ok && vu::CreateBuffer(*device, kWidth * kHeight * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                readback) == VK_SUCCESS;
    if (ok) {
        ok = vu::SubmitOneShot(*device, [&](VkCommandBuffer cmd) {
                 vu::ImageBarrier(*device, cmd, image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_ACCESS_TRANSFER_WRITE_BIT);
                 const VkClearColorValue color{.float32 = {1.0f, 0.0f, 1.0f, 1.0f}};
                 const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                 vk.vkCmdClearColorImage(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, &color, 1, &range);
                 vu::ImageBarrier(*device, cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                 const VkBufferImageCopy region{
                     .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .imageExtent = {kWidth, kHeight, 1},
                 };
                 vk.vkCmdCopyImageToBuffer(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &region);
                 vu::GlobalBarrier(*device, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
             }) == VK_SUCCESS;
        CHECK(ok);
    }
    if (ok) {
        const auto *pixels = static_cast<const uint32_t *>(readback.mapped);
        uint32_t mismatches = 0;
        for (uint32_t i = 0; i < kWidth * kHeight; ++i) {
            mismatches += pixels[i] != 0xFFFF00FFu; // R=255, G=0, B=255, A=255 in memory order
        }
        CHECK(mismatches == 0);
    }

    vu::DestroyBuffer(*device, readback);
    vu::DestroyImage(*device, image);
    CloseAndCheckValidation(device);
}

TEST_CASE("Non-owning VulkanDevice works on external handles and leaves them alive", "[gpu]") {
    auto owner = OpenDevice();

    // Wrap through counting stubs, as a frontend's own get_*_proc_addr would be.
    ExternalDevice handles = HandlesOf(*owner);
    g_realGipa = handles.getInstanceProcAddr;
    g_realGdpa = handles.getDeviceProcAddr;
    g_gipaInstanceCalls = 0;
    g_gdpaCalls = 0;
    handles.getInstanceProcAddr = CountingGipa;
    handles.getDeviceProcAddr = CountingGdpa;
    std::string reason;
    auto view = VulkanDevice::WrapExternal(handles, &reason);
    // Hand volk back the real loader entry point for the rest of the process (the stub keeps forwarding meanwhile).
    volk::volkInitializeCustom(g_realGipa);
    volk::volkLoadInstanceOnly(owner->Instance());
    REQUIRE(view != nullptr);
    CHECK(g_gipaInstanceCalls.load() > 0); // instance-level functions reloaded through the frontend's GIPA
    CHECK(g_gdpaCalls.load() > 0);         // device table loaded through the frontend's GDPA
    CHECK_FALSE(view->OwnsDevice());
    CHECK(view->Device() == owner->Device());
    CHECK(view->Queue() == owner->Queue());
    CHECK(view->QueueFamily() == owner->QueueFamily());
    CHECK(view->Features().apiVersion >= kRequiredApiVersion);

    // Submit() holds the queue lock around vkQueueSubmit only: lock, submit, unlock, then the fence wait.
    std::vector<char> events;
    view->SetQueueLockHooks([&] { events.push_back('L'); }, [&] { events.push_back('U'); });
    CheckFill(RunFill(*view, 1000));
    CHECK(events == std::vector<char>{'L', 'U'});

    view.reset();

    // The device, queue and instance are still usable through the owner.
    CheckFill(RunFill(*owner, 257));
    CloseAndCheckValidation(owner);
}

// A frontend context reset (context_destroy, then context_reset on a new instance that may reuse the old address).
TEST_CASE("Wrapping a device created after another was destroyed reloads volk", "[gpu]") {
    auto first = OpenDevice();
    const VkInstance firstInstance = first->Instance();
    CloseAndCheckValidation(first);
    // The destroyed instance no longer backs volk's instance-level functions.
    CHECK(volk::volkGetLoadedInstance() != firstInstance);

    auto second = OpenDevice();
    // Point volk at nothing, as after a frontend tore its instance down; WrapExternal must reload regardless.
    volk::volkLoadInstanceOnly(VK_NULL_HANDLE);
    std::string reason;
    auto view = VulkanDevice::WrapExternal(HandlesOf(*second), &reason);
    REQUIRE(view != nullptr);
    CHECK(volk::volkGetLoadedInstance() == second->Instance());
    CheckFill(RunFill(*view, 1000));
    view.reset();
    CloseAndCheckValidation(second);
}

TEST_CASE("Headless device selection honours preferredDeviceName", "[gpu]") {
    auto device = OpenDevice();
    const std::string name = device->Properties().deviceName;
    CloseAndCheckValidation(device);

    std::string reason;
    auto none = CreateHeadlessDevice({.validation = true, .preferredDeviceName = "no-such-gpu-xyzzy"}, &reason);
    CHECK(none == nullptr);
    CHECK_THAT(reason, Catch::Matchers::ContainsSubstring("no-such-gpu-xyzzy"));

    auto same = OpenDevice({.validation = true, .preferredDeviceName = name.substr(0, name.size() / 2 + 1)});
    CHECK(std::string(same->Properties().deviceName) == name);
    CloseAndCheckValidation(same);
}

TEST_CASE("Validation layer reports no errors over a device's lifetime", "[gpu]") {
    auto device = OpenDevice();
    if (!device->ValidationEnabled()) {
        SKIP("VK_LAYER_KHRONOS_validation is not installed");
    }
    auto errors = device->ValidationErrors();
    CheckFill(RunFill(*device, 64));
    CHECK(errors->load() == 0);
    device.reset();
    CHECK(errors->load() == 0);
}

TEST_CASE("Validation error counter counts error messages", "[gpu]") {
    auto device = OpenDevice();
    if (!device->ValidationEnabled()) {
        SKIP("VK_LAYER_KHRONOS_validation is not installed");
    }
    auto errors = device->ValidationErrors();
    REQUIRE(errors->load() == 0);

    VkDebugUtilsMessengerCallbackDataEXT data{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT};
    data.pMessageIdName = "brimir-test";
    data.pMessage = "deliberate test error (expected)";
    volk::vkSubmitDebugUtilsMessageEXT(device->Instance(), VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, &data);
    CHECK(errors->load() == 1);
    // General (loader) errors are printed but not counted.
    data.pMessage = "deliberate general error (expected, not counted)";
    volk::vkSubmitDebugUtilsMessageEXT(device->Instance(), VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                                       VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &data);
    CHECK(errors->load() == 1);
    device.reset();
    CHECK(errors->load() == 1);
}

namespace {

// A sync-submit rig as VulkanOutput::Impl::SyncSubmit has it: one command pool, command buffer and fence, reused; each
// submit copies `bytes` between two device-local buffers (a readback's size) and then waits with `wait`.
struct SyncRig {
    VulkanDevice &device;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    vk_util::Buffer src, dst;

    SyncRig(VulkanDevice &d, VkDeviceSize bytes)
        : device(d) {
        const auto &vk = device.Table();
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = device.QueueFamily();
        REQUIRE(vk.vkCreateCommandPool(device.Device(), &poolInfo, nullptr, &pool) == VK_SUCCESS);
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 1;
        REQUIRE(vk.vkAllocateCommandBuffers(device.Device(), &allocInfo, &cmd) == VK_SUCCESS);
        const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        REQUIRE(vk.vkCreateFence(device.Device(), &fenceInfo, nullptr, &fence) == VK_SUCCESS);
        REQUIRE(vk_util::CreateBuffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, src) == VK_SUCCESS);
        REQUIRE(vk_util::CreateBuffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, dst) == VK_SUCCESS);
    }
    ~SyncRig() {
        const auto &vk = device.Table();
        vk.vkDeviceWaitIdle(device.Device());
        vk_util::DestroyBuffer(device, src);
        vk_util::DestroyBuffer(device, dst);
        vk.vkDestroyFence(device.Device(), fence, nullptr);
        vk.vkDestroyCommandPool(device.Device(), pool, nullptr);
    }

    // Submits the copy `copies` times in one command buffer; returns the wait's result.
    VkResult Submit(uint32_t copies, const std::function<VkResult()> &wait) {
        const auto &vk = device.Table();
        if (vk.vkResetCommandPool(device.Device(), pool, 0) != VK_SUCCESS) {
            return VK_ERROR_UNKNOWN;
        }
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vk.vkBeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
            return VK_ERROR_UNKNOWN;
        }
        const VkBufferCopy region{0, 0, src.size};
        for (uint32_t i = 0; i < copies; ++i) {
            vk.vkCmdCopyBuffer(cmd, src.buffer, dst.buffer, 1, &region);
            vk_util::GlobalBarrier(device, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        }
        if (vk.vkEndCommandBuffer(cmd) != VK_SUCCESS || vk.vkResetFences(device.Device(), 1, &fence) != VK_SUCCESS) {
            return VK_ERROR_UNKNOWN;
        }
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        if (device.Submit(submit, fence) != VK_SUCCESS) {
            return VK_ERROR_UNKNOWN;
        }
        return wait();
    }
};

} // namespace

TEST_CASE("WaitForFence returns once the fence signals, short and long work alike", "[gpu]") {
    auto device = OpenDevice();
    {
        SyncRig rig(*device, 256 * 1024);
        const auto &vk = device->Table();
        // Long work (2000 copies of 256 KiB, milliseconds) with no budget or a short one: the poll gives up and the
        // blocking wait runs; short work with a generous budget: the poll alone sees it signal.
        bool blocked = false;
        CHECK(rig.Submit(2000, [&] { return vk_util::WaitForFence(*device, rig.fence, 0, UINT64_MAX, &blocked); }) ==
              VK_SUCCESS);
        CHECK(vk.vkGetFenceStatus(device->Device(), rig.fence) == VK_SUCCESS);
        CHECK(blocked);
        CHECK(rig.Submit(2000, [&] { return vk_util::WaitForFence(*device, rig.fence, 50, UINT64_MAX, &blocked); }) ==
              VK_SUCCESS);
        CHECK(vk.vkGetFenceStatus(device->Device(), rig.fence) == VK_SUCCESS);
        CHECK(blocked);
        CHECK(rig.Submit(1, [&] {
            return vk_util::WaitForFence(*device, rig.fence, 2'000'000, UINT64_MAX, &blocked); // 2 s budget
        }) == VK_SUCCESS);
        CHECK(vk.vkGetFenceStatus(device->Device(), rig.fence) == VK_SUCCESS);
        CHECK_FALSE(blocked);
        // An already signalled fence: the first poll returns
        CHECK(vk_util::WaitForFence(*device, rig.fence, 2000, UINT64_MAX, &blocked) == VK_SUCCESS);
        CHECK_FALSE(blocked);
        // A timeout on an unsignalled fence that nothing will signal
        REQUIRE(vk.vkResetFences(device->Device(), 1, &rig.fence) == VK_SUCCESS);
        CHECK(vk_util::WaitForFence(*device, rig.fence, 100, 1'000'000, &blocked) == VK_TIMEOUT);
        CHECK(blocked);
    }
    CloseAndCheckValidation(device);
}

TEST_CASE("Sync submit latency: blocking wait against WaitForFence", "[gpu][timing][.]") {
    // A 256 KiB copy (one FBRAM slice's readback) submitted and waited for, 200 times each way; prints the mean and
    // the worst latency from the submit to the wait's return, with 0, 2 and 8 ms of CPU work between the submits.
    auto device = OpenDevice({.validation = false});
    {
        SyncRig rig(*device, 256 * 1024);
        const auto &vk = device->Table();
        const auto measure = [&](const char *name, uint32_t idleMs, const std::function<VkResult()> &wait) {
            double total = 0, worst = 0;
            for (int i = 0; i < 200; ++i) {
                const auto t0 = std::chrono::steady_clock::now();
                REQUIRE(rig.Submit(1, wait) == VK_SUCCESS);
                const auto t1 = std::chrono::steady_clock::now();
                const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                total += ms;
                worst = std::max(worst, ms);
                // Emulation runs between the reads of a game: the GPU goes idle
                const auto idle = std::chrono::steady_clock::now();
                while (std::chrono::steady_clock::now() - idle < std::chrono::milliseconds(idleMs)) {
                }
            }
            std::printf("sync submit (%s), %s, %u ms between: mean %.3f ms, worst %.3f ms\n",
                        device->Properties().deviceName, name, idleMs, total / 200, worst);
        };
        for (const uint32_t idleMs : {0u, 2u, 8u}) {
            measure("vkWaitForFences", idleMs, [&] {
                return vk.vkWaitForFences(device->Device(), 1, &rig.fence, VK_TRUE, UINT64_MAX);
            });
            measure("WaitForFence (2 ms polled)", idleMs, [&] { return vk_util::WaitForFence(*device, rig.fence); });
        }
    }
    CloseAndCheckValidation(device);
}
