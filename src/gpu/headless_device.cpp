#include "headless_device.hpp"

#include "gpu_version.hpp"
#include "vk_util.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

namespace brimir::gpu {

namespace {

constexpr const char *kValidationLayer = "VK_LAYER_KHRONOS_validation";
// Must be enabled on devices that expose it (MoltenVK); it adds no functionality the renderer uses.
constexpr const char *kPortabilitySubset = "VK_KHR_portability_subset";

void SetReason(std::string *reason, std::string text) {
    if (reason) {
        *reason = std::move(text);
    }
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string VersionString(uint32_t version) {
    return std::to_string(VK_API_VERSION_MAJOR(version)) + "." + std::to_string(VK_API_VERSION_MINOR(version));
}

std::string Failed(const char *what, VkResult result) {
    return std::string(what) + " failed: " + vk_util::ResultName(result);
}

bool HasLayer(const char *name) {
    uint32_t count = 0;
    if (volk::vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkLayerProperties> layers(count);
    if (volk::vkEnumerateInstanceLayerProperties(&count, layers.data()) < VK_SUCCESS) {
        return false;
    }
    layers.resize(count);
    return std::any_of(layers.begin(), layers.end(),
                       [&](const VkLayerProperties &layer) { return std::strcmp(layer.layerName, name) == 0; });
}

bool HasInstanceExtension(const char *layer, const char *name) {
    uint32_t count = 0;
    if (volk::vkEnumerateInstanceExtensionProperties(layer, &count, nullptr) != VK_SUCCESS) {
        return false;
    }
    std::vector<VkExtensionProperties> extensions(count);
    if (volk::vkEnumerateInstanceExtensionProperties(layer, &count, extensions.data()) < VK_SUCCESS) {
        return false;
    }
    extensions.resize(count);
    return std::any_of(extensions.begin(), extensions.end(),
                       [&](const VkExtensionProperties &ext) { return std::strcmp(ext.extensionName, name) == 0; });
}

VKAPI_ATTR VkBool32 VKAPI_CALL OnDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT types,
                                              const VkDebugUtilsMessengerCallbackDataEXT *data, void *userData) {
    const bool error = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0;
    // Only validation errors count. The loader reports its own problems (for example a third-party layer's broken
    // manifest) as general messages; those are about the machine, not about Brimir's API use.
    const bool validation = (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0;
    if (error && validation) {
        static_cast<std::atomic<uint32_t> *>(userData)->fetch_add(1);
    }
    std::fprintf(stderr, "[vulkan] %s %s: %s\n", validation ? "validation" : "general", error ? "error" : "warning",
                 data && data->pMessage ? data->pMessage : "(no message)");
    return VK_FALSE;
}

struct Candidate {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    std::string name;
    uint32_t queueFamily = 0;
    bool portabilitySubset = false;
    std::string rejection; // empty when the device meets the baseline
};

Candidate Evaluate(VkPhysicalDevice physical) {
    Candidate candidate;
    candidate.physical = physical;

    VkPhysicalDeviceProperties properties{};
    volk::vkGetPhysicalDeviceProperties(physical, &properties);
    candidate.name = properties.deviceName;
    if (properties.apiVersion < kRequiredApiVersion) {
        candidate.rejection = "Vulkan " + VersionString(properties.apiVersion) + " < " +
                              VersionString(kRequiredApiVersion);
        return candidate;
    }

    uint32_t familyCount = 0;
    volk::vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    volk::vkGetPhysicalDeviceQueueFamilyProperties(physical, &familyCount, families.data());
    std::optional<uint32_t> family;
    for (uint32_t i = 0; i < familyCount; ++i) {
        const VkQueueFlags flags = families[i].queueFlags;
        if ((flags & VK_QUEUE_COMPUTE_BIT) && families[i].queueCount > 0) {
            // Like a libretro frontend's queue: graphics + compute when the device has one.
            if (!family || (flags & VK_QUEUE_GRAPHICS_BIT)) {
                family = i;
            }
            if (flags & VK_QUEUE_GRAPHICS_BIT) {
                break;
            }
        }
    }
    if (!family) {
        candidate.rejection = "no compute queue";
        return candidate;
    }
    candidate.queueFamily = *family;

    VkPhysicalDeviceFeatures features{};
    volk::vkGetPhysicalDeviceFeatures(physical, &features);
    for (const CoreFeature &feature : kRequiredCoreFeatures) {
        if (!(features.*feature.member)) {
            candidate.rejection = std::string("missing feature ") + feature.name;
            return candidate;
        }
    }

    uint32_t extensionCount = 0;
    volk::vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    volk::vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, extensions.data());
    extensions.resize(extensionCount);
    const auto hasExtension = [&](const char *name) {
        return std::any_of(extensions.begin(), extensions.end(),
                           [&](const VkExtensionProperties &ext) { return std::strcmp(ext.extensionName, name) == 0; });
    };
    for (const char *extension : kRequiredDeviceExtensions) {
        if (!hasExtension(extension)) {
            candidate.rejection = std::string("missing extension ") + extension;
            return candidate;
        }
    }
    candidate.portabilitySubset = hasExtension(kPortabilitySubset);
    return candidate;
}

} // namespace

struct HeadlessDeviceAccess {
    static std::unique_ptr<VulkanDevice> Create(const HeadlessOptions &options, std::string *reason);
};

std::unique_ptr<VulkanDevice> HeadlessDeviceAccess::Create(const HeadlessOptions &options, std::string *reason) {
    // volkInitialize() once per process; a second call would load the loader library again.
    if (!volk::vkGetInstanceProcAddr && volk::volkInitialize() != VK_SUCCESS) {
        SetReason(reason, "the Vulkan loader (vulkan-1.dll / libvulkan.so.1 / libvulkan.1.dylib) was not found");
        return nullptr;
    }
    const uint32_t loaderVersion = volk::volkGetInstanceVersion();
    if (loaderVersion < kRequiredApiVersion) {
        SetReason(reason, "the Vulkan loader supports only Vulkan " + VersionString(loaderVersion));
        return nullptr;
    }

    // The owning VulkanDevice exists from the start: its destructor cleans up whatever was created on every path.
    std::unique_ptr<VulkanDevice> device(new VulkanDevice());
    device->m_owning = true;

    std::vector<const char *> layers;
    std::vector<const char *> instanceExtensions;
    bool debugUtils = false;
    if (options.validation) {
        if (HasLayer(kValidationLayer)) {
            layers.push_back(kValidationLayer);
            debugUtils = HasInstanceExtension(nullptr, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) ||
                         HasInstanceExtension(kValidationLayer, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            if (debugUtils) {
                instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            }
        } else {
            std::fprintf(stderr, "[vulkan] %s is not installed; running without validation\n", kValidationLayer);
        }
    }
    VkInstanceCreateFlags instanceFlags = 0;
    if (HasInstanceExtension(nullptr, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        instanceExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        instanceFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }

    VkDebugUtilsMessengerCreateInfoEXT messengerInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messengerInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messengerInfo.pfnUserCallback = OnDebugMessage;
    messengerInfo.pUserData = device->m_validationErrors.get();

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "brimir-headless";
    appInfo.pEngineName = "Brimir";
    appInfo.apiVersion = kRequiredApiVersion;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    // Chained so instance creation and destruction are validated too.
    instanceInfo.pNext = debugUtils ? &messengerInfo : nullptr;
    instanceInfo.flags = instanceFlags;
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.data();
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames = instanceExtensions.data();

    VkInstance instance = VK_NULL_HANDLE;
    if (const VkResult result = volk::vkCreateInstance(&instanceInfo, nullptr, &instance); result != VK_SUCCESS) {
        SetReason(reason, Failed("vkCreateInstance", result));
        return nullptr;
    }
    volk::volkLoadInstanceOnly(instance);
    device->m_instance = instance;
    device->m_destroyInstance = volk::vkDestroyInstance;

    if (debugUtils) {
        if (const VkResult result =
                volk::vkCreateDebugUtilsMessengerEXT(instance, &messengerInfo, nullptr, &device->m_messenger);
            result != VK_SUCCESS) {
            SetReason(reason, Failed("vkCreateDebugUtilsMessengerEXT", result));
            return nullptr;
        }
        device->m_destroyMessenger = volk::vkDestroyDebugUtilsMessengerEXT;
        device->m_validationEnabled = true;
    }

    uint32_t physicalCount = 0;
    if (const VkResult result = volk::vkEnumeratePhysicalDevices(instance, &physicalCount, nullptr);
        result != VK_SUCCESS) {
        SetReason(reason, Failed("vkEnumeratePhysicalDevices", result));
        return nullptr;
    }
    std::vector<VkPhysicalDevice> physicals(physicalCount);
    if (const VkResult result = volk::vkEnumeratePhysicalDevices(instance, &physicalCount, physicals.data());
        result < VK_SUCCESS) {
        SetReason(reason, Failed("vkEnumeratePhysicalDevices", result));
        return nullptr;
    }
    physicals.resize(physicalCount);
    if (physicals.empty()) {
        SetReason(reason, "no Vulkan physical devices");
        return nullptr;
    }

    std::string wanted = options.preferredDeviceName;
    const char *wantedSource = "preferredDeviceName";
    if (wanted.empty()) {
        if (const char *env = std::getenv("BRIMIR_VK_DEVICE"); env && *env) {
            wanted = env;
            wantedSource = "BRIMIR_VK_DEVICE";
        }
    }

    std::optional<Candidate> chosen;
    std::string considered;
    for (VkPhysicalDevice physical : physicals) {
        Candidate candidate = Evaluate(physical);
        if (!wanted.empty() && Lower(candidate.name).find(Lower(wanted)) == std::string::npos) {
            considered += (considered.empty() ? "" : "; ") + candidate.name + ": name does not match";
            continue;
        }
        if (!candidate.rejection.empty()) {
            considered += (considered.empty() ? "" : "; ") + candidate.name + ": " + candidate.rejection;
            continue;
        }
        chosen = std::move(candidate);
        break;
    }
    if (!chosen) {
        std::string text = "no Vulkan device meets the baseline";
        if (!wanted.empty()) {
            text += " and matches \"" + wanted + "\" (" + wantedSource + ")";
        }
        SetReason(reason, text + " (" + considered + ")");
        return nullptr;
    }

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = chosen->queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceFeatures enabledFeatures{};
    for (const CoreFeature &feature : kRequiredCoreFeatures) {
        enabledFeatures.*feature.member = VK_TRUE;
    }
    std::vector<const char *> deviceExtensions(kRequiredDeviceExtensions.begin(), kRequiredDeviceExtensions.end());
    if (chosen->portabilitySubset) {
        deviceExtensions.push_back(kPortabilitySubset);
    }

    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
    deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
    deviceInfo.pEnabledFeatures = &enabledFeatures;

    VkDevice vkDevice = VK_NULL_HANDLE;
    if (const VkResult result = volk::vkCreateDevice(chosen->physical, &deviceInfo, nullptr, &vkDevice);
        result != VK_SUCCESS) {
        SetReason(reason, Failed("vkCreateDevice", result) + " on " + chosen->name);
        return nullptr;
    }
    device->m_device = vkDevice;
    volk::volkLoadDeviceTable(&device->m_table, vkDevice);

    VkQueue queue = VK_NULL_HANDLE;
    device->m_table.vkGetDeviceQueue(vkDevice, chosen->queueFamily, 0, &queue);

    ExternalDevice handles;
    handles.instance = instance;
    handles.physical = chosen->physical;
    handles.device = vkDevice;
    handles.queue = queue;
    handles.queueFamily = chosen->queueFamily;
    handles.getInstanceProcAddr = volk::vkGetInstanceProcAddr;
    handles.apiVersion = kRequiredApiVersion;
    handles.enabledFeatures = enabledFeatures;
    if (!device->Init(handles, reason)) {
        return nullptr;
    }
    return device;
}

std::unique_ptr<VulkanDevice> CreateHeadlessDevice(const HeadlessOptions &options, std::string *reason) {
    return HeadlessDeviceAccess::Create(options, reason);
}

} // namespace brimir::gpu
