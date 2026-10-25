// brimir-gpu device layer: the headless device, the owning and non-owning VulkanDevice and the vk_util compute helpers.
// Every case runs on a headless device with the Khronos validation layer when it is installed, fails on any
// validation error (including leaks reported at device destruction) and SKIPs when there is no usable Vulkan device
// (fails instead with BRIMIR_REQUIRE_VULKAN=1).
#include "catch_amalgamated.hpp"

#include <gpu/gpu_version.hpp>
#include <gpu/headless_device.hpp>
#include <gpu/shaders/generated/cs_fill_test.spv.h>
#include <gpu/vk_device.hpp>
#include <gpu/vk_util.hpp>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace brimir::gpu;
namespace vu = brimir::gpu::vk_util;

namespace {

// BRIMIR_REQUIRE_VULKAN=1 (the gpu-lavapipe CI job) turns a missing device or validation layer into a failure.
bool RequireVulkan() {
    const char *env = std::getenv("BRIMIR_REQUIRE_VULKAN");
    return env && std::strcmp(env, "1") == 0;
}

std::unique_ptr<VulkanDevice> OpenDevice(HeadlessOptions options = {.validation = true}) {
    std::string reason;
    auto device = CreateHeadlessDevice(options, &reason);
    if (!device) {
        if (RequireVulkan()) {
            FAIL("no Vulkan device: " << reason);
        }
        SKIP("no Vulkan device: " << reason);
    }
    if (RequireVulkan() && options.validation && !device->ValidationEnabled()) {
        FAIL("VK_LAYER_KHRONOS_validation is not installed");
    }
    return device;
}

// Destroys the device (validation reports leaks then) and checks that no validation error was reported at all.
void CloseAndCheckValidation(std::unique_ptr<VulkanDevice> &device) {
    const bool validation = device->ValidationEnabled();
    auto errors = device->ValidationErrors();
    device.reset();
    if (validation) {
        CHECK(errors->load() == 0);
    }
}

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

    ExternalDevice handles{
        .instance = owner->Instance(),
        .physical = owner->Physical(),
        .device = owner->Device(),
        .queue = owner->Queue(),
        .queueFamily = owner->QueueFamily(),
        .getInstanceProcAddr = volk::vkGetInstanceProcAddr,
        .getDeviceProcAddr = volk::vkGetDeviceProcAddr,
        .apiVersion = kRequiredApiVersion,
    };
    std::string reason;
    auto view = VulkanDevice::WrapExternal(handles, &reason);
    REQUIRE(view != nullptr);
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
