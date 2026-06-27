#include "Graphics/Vulkan/VulkanCanvas.hpp"

#include <Lambda/Debug/PerfCounters.hpp>
#include <Lambda/Graphics/Image.hpp>
#include <Lambda/Graphics/RenderTarget.hpp>
#include <Lambda/Graphics/TextSystem.hpp>

#include "Graphics/CanvasGeometry.hpp"
#include "Graphics/PathFlattener.hpp"
#include "Graphics/Vulkan/VulkanCanvasTypes.hpp"
#include "Graphics/Vulkan/VulkanCheck.hpp"
#include "Graphics/Vulkan/VulkanContextPrivate.hpp"
#include "Graphics/Vulkan/VulkanDrawCommands.hpp"
#include "Graphics/Vulkan/VulkanFrameRecorder.hpp"
#include "Graphics/Vulkan/VulkanImage.hpp"
#include "Graphics/Vulkan/VulkanGlyphAtlas.hpp"
#include "Graphics/Vulkan/VulkanPipelines.hpp"
#include "Graphics/Vulkan/VulkanSwapchain.hpp"
#include "Detail/ResizeTrace.hpp"

#include <cassert>
#include <drm_fourcc.h>
#include <vulkan/vulkan.h>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif
#include "vma/vk_mem_alloc.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>

namespace lambdaui {

namespace {

class VulkanCanvas;

constexpr std::size_t kMaxFramesInFlight = kVulkanMaxFramesInFlight;
constexpr int kBackdropBlurIterations = 2;
constexpr std::uint32_t kDefaultBackdropBlurBaseDownsample = 2;
constexpr std::uint32_t kMinBackdropBlurBaseDownsample = 1;
constexpr std::uint32_t kMaxBackdropBlurBaseDownsample = 8;
constexpr std::uint32_t kMaxBackdropBlurDownsample = 16;
constexpr float kBackdropBlurRadiusBoost = 1.35f;

bool renderTargetFrameCacheDisabled() {
  static bool const disabled = [] {
    char const* value = std::getenv("LAMBDA_RENDER_TARGET_DISABLE_FRAME_CACHE");
    return debug::envNonZero(value);
  }();
  return disabled;
}

bool vulkanPresentFencesEnabled() {
  static bool const enabled = [] {
    char const* value = std::getenv("LAMBDA_VULKAN_PRESENT_FENCES");
    return debug::envNonZero(value);
  }();
  return enabled;
}

bool vulkanForceFifoPresentMode() {
  static bool const enabled = [] {
    char const* value = std::getenv("LAMBDA_VULKAN_FORCE_FIFO_PRESENT_MODE");
    return debug::envNonZero(value);
  }();
  return enabled;
}

int vulkanPreparedGeometryOverride() {
  static int const overrideValue = [] {
    char const* value = std::getenv("LAMBDA_VULKAN_PREPARED_GEOMETRY");
    if (!value || !*value) {
      return 0;
    }
    if (*value == '0' || std::strcmp(value, "false") == 0 || std::strcmp(value, "off") == 0) {
      return -1;
    }
    if (*value == '1' || std::strcmp(value, "true") == 0 || std::strcmp(value, "on") == 0 ||
        std::strcmp(value, "force") == 0) {
      return 1;
    }
    return 0;
  }();
  return overrideValue;
}


struct ImageBatch {
  Texture *texture = nullptr;
  std::uint32_t first = 0;
  std::uint32_t count = 0;
};

struct PathCacheKey {
  std::uint64_t pathHash = 0;
  std::uint64_t styleHash = 0;
  int viewportW = 0;
  int viewportH = 0;

  bool operator==(PathCacheKey const &) const = default;
};

struct PathCacheKeyHash {
  std::size_t operator()(PathCacheKey const &key) const noexcept {
    std::size_t h = static_cast<std::size_t>(key.pathHash);
    h ^= static_cast<std::size_t>(key.styleHash + 0x9e3779b97f4a7c15ULL + (h << 6u) + (h >> 2u));
    h ^= static_cast<std::size_t>(key.viewportW) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
    h ^= static_cast<std::size_t>(key.viewportH) + 0x9e3779b9u + (h << 6u) + (h >> 2u);
    return h;
  }
};

struct CachedPath {
  std::vector<VulkanPathVertex> vertices;
  std::list<PathCacheKey>::iterator lruIt;
};

void putColor(float out[4], Color c, float opacity = 1.f) {
  out[0] = c.r;
  out[1] = c.g;
  out[2] = c.b;
  out[3] = c.a * opacity;
}

std::uint64_t mixHashWord(std::uint64_t value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27u)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31u);
}

void hashBytes(std::uint64_t &h, void const *data, std::size_t size) {
  auto const *bytes = static_cast<std::uint8_t const *>(data);
  while (size >= sizeof(std::uint64_t)) {
    std::uint64_t word = 0;
    std::memcpy(&word, bytes, sizeof(word));
    h ^= mixHashWord(word);
    h *= 1099511628211ULL;
    bytes += sizeof(word);
    size -= sizeof(word);
  }
  if (size > 0) {
    std::uint64_t tail = 0;
    std::memcpy(&tail, bytes, size);
    h ^= mixHashWord(tail ^ size);
    h *= 1099511628211ULL;
  }
}

template <typename T>
void hashValue(std::uint64_t &h, T const &value) {
  hashBytes(h, &value, sizeof(value));
}

void hashColor(std::uint64_t &h, Color c) {
  hashValue(h, c.r);
  hashValue(h, c.g);
  hashValue(h, c.b);
  hashValue(h, c.a);
}

void hashPoint(std::uint64_t &h, Point p) {
  hashValue(h, p.x);
  hashValue(h, p.y);
}

void hashStops(std::uint64_t &h, std::array<GradientStop, kMaxGradientStops> const &stops, std::uint8_t count) {
  hashValue(h, count);
  for (std::uint8_t i = 0; i < count; ++i) {
    hashValue(h, stops[i].position);
    hashColor(h, stops[i].color);
  }
}

std::uint64_t hashFill(FillStyle const &fill) {
  std::uint64_t h = 14695981039346656037ULL;
  hashValue(h, fill.fillRule);
  hashValue(h, fill.data.index());
  Color solid{};
  if (fill.solidColor(&solid)) {
    hashColor(h, solid);
  }
  LinearGradient linear{};
  if (fill.linearGradient(&linear)) {
    hashPoint(h, linear.start);
    hashPoint(h, linear.end);
    hashStops(h, linear.stops, linear.stopCount);
  }
  RadialGradient radial{};
  if (fill.radialGradient(&radial)) {
    hashPoint(h, radial.center);
    hashValue(h, radial.radius);
    hashStops(h, radial.stops, radial.stopCount);
  }
  ConicalGradient conical{};
  if (fill.conicalGradient(&conical)) {
    hashPoint(h, conical.center);
    hashValue(h, conical.startAngleRadians);
    hashStops(h, conical.stops, conical.stopCount);
  }
  return h;
}

std::uint64_t hashStroke(StrokeStyle const &stroke) {
  std::uint64_t h = 14695981039346656037ULL;
  hashValue(h, stroke.type);
  hashColor(h, stroke.color);
  hashValue(h, stroke.width);
  hashValue(h, stroke.cap);
  hashValue(h, stroke.join);
  hashValue(h, stroke.miterLimit);
  return h;
}

std::uint64_t hashTransform(Mat3 const &transform, float opacity) {
  std::uint64_t h = 14695981039346656037ULL;
  for (float value : transform.m) {
    hashValue(h, value);
  }
  hashValue(h, opacity);
  return h;
}

Rect unionRects(Rect a, Rect b) {
  float const x0 = std::min(a.x, b.x);
  float const y0 = std::min(a.y, b.y);
  float const x1 = std::max(a.x + a.width, b.x + b.width);
  float const y1 = std::max(a.y + a.height, b.y + b.height);
  return Rect::sharp(x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0));
}

std::uint32_t findMemoryType(VkPhysicalDevice physical, std::uint32_t typeBits,
                             VkMemoryPropertyFlags requiredProperties) {
  VkPhysicalDeviceMemoryProperties props{};
  vkGetPhysicalDeviceMemoryProperties(physical, &props);
  for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((typeBits & (1u << i)) &&
        (props.memoryTypes[i].propertyFlags & requiredProperties) == requiredProperties) {
      return i;
    }
  }
  throw std::runtime_error("No suitable Vulkan memory type");
}

VkFormat vkFormatForDrmFormat(std::uint32_t drmFormat) {
  switch (drmFormat) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
      return VK_FORMAT_B8G8R8A8_UNORM;
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888:
      return VK_FORMAT_R8G8B8A8_UNORM;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

std::mutex gVulkanCoreMutex;
SharedVulkanCore gVulkanCore;
std::vector<std::string> gRequiredInstanceExtensions;
std::vector<std::string> gRequiredDeviceExtensions;
std::filesystem::path gPipelineCacheDir;
void destroySharedVulkanResources(SharedVulkanCore &core);

std::mutex gCanvasRegistryMutex;
std::vector<VulkanCanvas*> gCanvases;

bool containsExtension(std::vector<std::string> const &extensions, char const *name) {
  if (!name || !*name) {
    return true;
  }
  return std::any_of(extensions.begin(), extensions.end(), [name](std::string const &extension) {
    return extension == name;
  });
}

void appendUniqueExtension(std::vector<std::string> &extensions, char const *name) {
  if (!name || !*name || containsExtension(extensions, name)) {
    return;
  }
  extensions.emplace_back(name);
}

bool allExtensionsConfigured(std::vector<std::string> const &extensions,
                             std::span<char const* const> required) {
  for (char const *extension : required) {
    if (!containsExtension(extensions, extension)) {
      return false;
    }
  }
  return true;
}

std::vector<char const *> extensionNamePointers(std::vector<std::string> const &extensions) {
  std::vector<char const *> names;
  names.reserve(extensions.size());
  for (std::string const &extension : extensions) {
    names.push_back(extension.c_str());
  }
  return names;
}

std::string vulkanVersionString(std::uint32_t version) {
  return std::to_string(VK_VERSION_MAJOR(version)) + "." +
         std::to_string(VK_VERSION_MINOR(version)) + "." +
         std::to_string(VK_VERSION_PATCH(version));
}

bool extensionAvailable(std::vector<VkExtensionProperties> const &available, char const *required) {
  return std::any_of(available.begin(), available.end(), [&](VkExtensionProperties const &extension) {
    return std::strcmp(extension.extensionName, required) == 0;
  });
}

VkInstance ensureSharedVulkanInstanceImpl() {
  std::lock_guard lock(gVulkanCoreMutex);
  if (gVulkanCore.instance)
    return gVulkanCore.instance;
  auto app = vkStructure<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
  app.pApplicationName = "Lambda";
  app.apiVersion = VK_API_VERSION_1_3;
  if (containsExtension(gRequiredInstanceExtensions, VK_KHR_SURFACE_EXTENSION_NAME)) {
    std::uint32_t availableCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr);
    std::vector<VkExtensionProperties> available(availableCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, available.data());
    if (extensionAvailable(available, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) &&
        extensionAvailable(available, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME)) {
      appendUniqueExtension(gRequiredInstanceExtensions, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
      appendUniqueExtension(gRequiredInstanceExtensions, VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
    }
  }
  std::vector<char const *> instanceExtensions = extensionNamePointers(gRequiredInstanceExtensions);
  if (!instanceExtensions.empty()) {
    std::uint32_t availableCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr);
    std::vector<VkExtensionProperties> available(availableCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, available.data());
    for (char const *extension : instanceExtensions) {
      if (!extensionAvailable(available, extension)) {
        throw std::runtime_error(std::string("Missing required Vulkan instance extension: ") +
                                 extension);
      }
    }
  }
  auto info = vkStructure<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
  info.pApplicationInfo = &app;
  info.enabledExtensionCount = static_cast<std::uint32_t>(instanceExtensions.size());
  info.ppEnabledExtensionNames = instanceExtensions.empty() ? nullptr : instanceExtensions.data();
  vkCheck(vkCreateInstance(&info, nullptr, &gVulkanCore.instance), "vkCreateInstance");
  return gVulkanCore.instance;
}

std::string hexBytes(std::uint8_t const *bytes, std::size_t size) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < size; ++i) {
    out << std::setw(2) << static_cast<unsigned int>(bytes[i]);
  }
  return out.str();
}

std::filesystem::path pipelineCachePath(VkPhysicalDevice physical) {
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physical, &props);
  std::string identity;
  auto getProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      vkGetInstanceProcAddr(gVulkanCore.instance, "vkGetPhysicalDeviceProperties2"));
  if (!getProps2) {
    getProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        vkGetInstanceProcAddr(gVulkanCore.instance, "vkGetPhysicalDeviceProperties2KHR"));
  }
  if (getProps2) {
    auto idProps = vkStructure<VkPhysicalDeviceIDProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
    auto props2 = vkStructure<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
    props2.pNext = &idProps;
    getProps2(physical, &props2);
    identity = hexBytes(idProps.deviceUUID, VK_UUID_SIZE);
  }
  if (identity.empty() || identity == std::string(VK_UUID_SIZE * 2, '0')) {
    identity = std::to_string(props.vendorID) + "-" + std::to_string(props.deviceID);
  }
  std::filesystem::path cacheDir = gPipelineCacheDir.empty()
      ? std::filesystem::temp_directory_path()
      : gPipelineCacheDir;
  return cacheDir / ("lambda-vulkan-" + identity + ".cache");
}

void createPipelineCache(SharedVulkanCore &core) {
  auto &res = core.resources;
  if (res.pipelineCache)
    return;
  res.pipelineCacheFile = pipelineCachePath(core.physical);
  std::vector<std::uint8_t> initialData;
  if (std::ifstream in(res.pipelineCacheFile, std::ios::binary); in) {
    initialData.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  auto info = vkStructure<VkPipelineCacheCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO);
  info.initialDataSize = initialData.size();
  info.pInitialData = initialData.empty() ? nullptr : initialData.data();
  VkResult const result = vkCreatePipelineCache(core.device, &info, nullptr, &res.pipelineCache);
  if (result != VK_SUCCESS && !initialData.empty()) {
    info.initialDataSize = 0;
    info.pInitialData = nullptr;
    vkCheck(vkCreatePipelineCache(core.device, &info, nullptr, &res.pipelineCache),
            "vkCreatePipelineCache");
    return;
  }
  vkCheck(result, "vkCreatePipelineCache");
}

void saveAndDestroyPipelineCache(VkDevice device, SharedVulkanCore::Resources &res) {
  if (!res.pipelineCache)
    return;
  std::size_t size = 0;
  if (vkGetPipelineCacheData(device, res.pipelineCache, &size, nullptr) == VK_SUCCESS && size > 0) {
    std::vector<std::uint8_t> data(size);
    if (vkGetPipelineCacheData(device, res.pipelineCache, &size, data.data()) == VK_SUCCESS) {
      try {
        std::filesystem::create_directories(res.pipelineCacheFile.parent_path());
        std::ofstream out(res.pipelineCacheFile, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<char const *>(data.data()), static_cast<std::streamsize>(size));
      } catch (...) {
      }
    }
  }
  vkDestroyPipelineCache(device, res.pipelineCache, nullptr);
  res.pipelineCache = VK_NULL_HANDLE;
}

SharedVulkanCore *acquireSharedVulkanCore(VkSurfaceKHR surface) {
  std::lock_guard lock(gVulkanCoreMutex);
  if (!gVulkanCore.instance) {
    throw std::runtime_error("Vulkan instance was not initialized");
  }
  bool const needsPresent = surface != VK_NULL_HANDLE;
  if (!gVulkanCore.device) {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(gVulkanCore.instance, &count, nullptr);
    if (!count)
      throw std::runtime_error("No Vulkan physical devices");
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(gVulkanCore.instance, &count, devices.data());
    std::vector<std::string> rejectedDevices;
    for (VkPhysicalDevice d : devices) {
      VkPhysicalDeviceProperties props{};
      vkGetPhysicalDeviceProperties(d, &props);
      std::string const deviceName = props.deviceName[0] ? props.deviceName : "unnamed device";
      if (props.apiVersion < VK_API_VERSION_1_3) {
        rejectedDevices.push_back(deviceName + ": Vulkan 1.3 required, device exposes " +
                                  vulkanVersionString(props.apiVersion));
        continue;
      }
      auto vk13 = vkStructure<VkPhysicalDeviceVulkan13Features>(
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
      auto features = vkStructure<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
      features.pNext = &vk13;
      vkGetPhysicalDeviceFeatures2(d, &features);
      if (!vk13.dynamicRendering || !vk13.synchronization2) {
        std::string missing;
        if (!vk13.dynamicRendering) {
          missing += missing.empty() ? "dynamicRendering" : ", dynamicRendering";
        }
        if (!vk13.synchronization2) {
          missing += missing.empty() ? "synchronization2" : ", synchronization2";
        }
        rejectedDevices.push_back(deviceName + ": missing Vulkan 1.3 feature(s): " + missing);
        continue;
      }
      std::vector<std::string> deviceExtensions = gRequiredDeviceExtensions;
      if (needsPresent) {
        appendUniqueExtension(deviceExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
      }
      std::uint32_t extensionCount = 0;
      vkEnumerateDeviceExtensionProperties(d, nullptr, &extensionCount, nullptr);
      std::vector<VkExtensionProperties> availableExtensions(extensionCount);
      vkEnumerateDeviceExtensionProperties(d, nullptr, &extensionCount, availableExtensions.data());
      if (needsPresent && extensionAvailable(availableExtensions, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME)) {
        appendUniqueExtension(deviceExtensions, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
      }
      char const *swapchainMaintenanceExtension = nullptr;
      bool swapchainMaintenanceFeature = false;
      if (needsPresent) {
        if (extensionAvailable(availableExtensions, VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
          swapchainMaintenanceExtension = VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
        } else if (extensionAvailable(availableExtensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
          swapchainMaintenanceExtension = VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME;
        }
        if (swapchainMaintenanceExtension) {
          auto swapchainMaintenance = vkStructure<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>(
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR);
          auto maintenanceVk13 = vkStructure<VkPhysicalDeviceVulkan13Features>(
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
          maintenanceVk13.pNext = &swapchainMaintenance;
          auto maintenanceFeatures =
              vkStructure<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
          maintenanceFeatures.pNext = &maintenanceVk13;
          vkGetPhysicalDeviceFeatures2(d, &maintenanceFeatures);
          swapchainMaintenanceFeature = swapchainMaintenance.swapchainMaintenance1 == VK_TRUE;
          if (swapchainMaintenanceFeature) {
            appendUniqueExtension(deviceExtensions, swapchainMaintenanceExtension);
          }
        }
      }
      if (!deviceExtensions.empty()) {
        std::vector<std::string> missingExtensions;
        for (std::string const &extension : deviceExtensions) {
          if (!extensionAvailable(availableExtensions, extension.c_str())) {
            missingExtensions.push_back(extension);
          }
        }
        if (!missingExtensions.empty()) {
          std::string missing = missingExtensions.front();
          for (std::size_t i = 1; i < missingExtensions.size(); ++i) {
            missing += ", " + missingExtensions[i];
          }
          rejectedDevices.push_back(deviceName + ": missing Vulkan device extension(s): " + missing);
          continue;
        }
      }
      std::uint32_t familiesCount = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(d, &familiesCount, nullptr);
      std::vector<VkQueueFamilyProperties> families(familiesCount);
      vkGetPhysicalDeviceQueueFamilyProperties(d, &familiesCount, families.data());
      bool hasGraphicsQueue = false;
      bool hasGraphicsPresentQueue = false;
      for (std::uint32_t i = 0; i < familiesCount; ++i) {
        VkBool32 present = VK_FALSE;
        if (needsPresent) {
          vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface, &present);
        }
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
          hasGraphicsQueue = true;
          hasGraphicsPresentQueue = hasGraphicsPresentQueue || !needsPresent || present == VK_TRUE;
        }
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (!needsPresent || present == VK_TRUE)) {
          gVulkanCore.physical = d;
          gVulkanCore.queueFamily = i;
          float priority = 1.f;
          auto q = vkStructure<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
          q.queueFamilyIndex = i;
          q.queueCount = 1;
          q.pQueuePriorities = &priority;
          auto enabled13 = vkStructure<VkPhysicalDeviceVulkan13Features>(
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES);
          enabled13.synchronization2 = VK_TRUE;
          enabled13.dynamicRendering = VK_TRUE;
          auto enabledSwapchainMaintenance = vkStructure<VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR>(
              VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR);
          if (swapchainMaintenanceFeature) {
            enabledSwapchainMaintenance.swapchainMaintenance1 = VK_TRUE;
            enabled13.pNext = &enabledSwapchainMaintenance;
          }
          std::vector<char const *> deviceExtensionNames = extensionNamePointers(deviceExtensions);
          auto info = vkStructure<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
          info.pNext = &enabled13;
          info.queueCreateInfoCount = 1;
          info.pQueueCreateInfos = &q;
          info.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensionNames.size());
          info.ppEnabledExtensionNames = deviceExtensionNames.empty() ? nullptr : deviceExtensionNames.data();
          auto driverProps =
              vkStructure<VkPhysicalDeviceDriverProperties>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES);
          auto props2 = vkStructure<VkPhysicalDeviceProperties2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2);
          props2.pNext = &driverProps;
          vkGetPhysicalDeviceProperties2(gVulkanCore.physical, &props2);
          gVulkanCore.driverId = driverProps.driverID;
          gVulkanCore.driverName = driverProps.driverName;
          gVulkanCore.driverInfo = driverProps.driverInfo;
          vkCheck(vkCreateDevice(gVulkanCore.physical, &info, nullptr, &gVulkanCore.device), "vkCreateDevice");
          gVulkanCore.googleDisplayTiming =
              containsExtension(deviceExtensions, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);
          gVulkanCore.swapchainMaintenance1 =
              swapchainMaintenanceFeature && swapchainMaintenanceExtension != nullptr &&
              containsExtension(deviceExtensions, swapchainMaintenanceExtension);
          gVulkanCore.swapchainMaintenance1Extension =
              gVulkanCore.swapchainMaintenance1 ? swapchainMaintenanceExtension : "";
          if (needsPresent) {
            std::fprintf(stderr, "Lambda Vulkan: swapchain maintenance1 %s%s%s\n",
                         gVulkanCore.swapchainMaintenance1 ? "enabled" : "unavailable",
                         gVulkanCore.swapchainMaintenance1 ? " via " : "",
                         gVulkanCore.swapchainMaintenance1
                             ? gVulkanCore.swapchainMaintenance1Extension.c_str()
                             : "");
          }
          VmaAllocatorCreateInfo allocatorInfo{};
          allocatorInfo.physicalDevice = gVulkanCore.physical;
          allocatorInfo.device = gVulkanCore.device;
          allocatorInfo.instance = gVulkanCore.instance;
          allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3;
          vkCheck(vmaCreateAllocator(&allocatorInfo, &gVulkanCore.allocator), "vmaCreateAllocator");
          vkGetDeviceQueue(gVulkanCore.device, gVulkanCore.queueFamily, 0, &gVulkanCore.queue);
          ++gVulkanCore.refs;
          return &gVulkanCore;
        }
      }
      if (!hasGraphicsQueue) {
        rejectedDevices.push_back(deviceName + ": no graphics queue family");
      } else if (needsPresent && !hasGraphicsPresentQueue) {
        rejectedDevices.push_back(deviceName + ": no graphics queue family can present to this surface");
      }
    }
    std::string message = needsPresent ? "No suitable Vulkan graphics/present device"
                                       : "No suitable Vulkan graphics device";
    if (!rejectedDevices.empty()) {
      message += ": ";
      for (std::size_t i = 0; i < rejectedDevices.size(); ++i) {
        if (i > 0) {
          message += "; ";
        }
        message += rejectedDevices[i];
      }
    }
    throw std::runtime_error(message);
  }
  if (needsPresent) {
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(gVulkanCore.physical, gVulkanCore.queueFamily, surface, &present);
    if (!present) {
      throw std::runtime_error("Shared Vulkan queue cannot present to this surface");
    }
  }
  ++gVulkanCore.refs;
  return &gVulkanCore;
}

void releaseSharedVulkanCore() {
  std::lock_guard lock(gVulkanCoreMutex);
  if (gVulkanCore.refs == 0)
    return;
  --gVulkanCore.refs;
  if (gVulkanCore.refs != 0)
    return;
  if (gVulkanCore.device) {
    // Intentional shutdown-only wait before destroying shared Vulkan objects.
    vkDeviceWaitIdle(gVulkanCore.device);
    destroySharedVulkanResources(gVulkanCore);
    if (gVulkanCore.allocator) {
      vmaDestroyAllocator(gVulkanCore.allocator);
      gVulkanCore.allocator = VK_NULL_HANDLE;
    }
    vkDestroyDevice(gVulkanCore.device, nullptr);
  }
  if (gVulkanCore.instance) {
    vkDestroyInstance(gVulkanCore.instance, nullptr);
  }
  gVulkanCore = {};
}

void destroySharedTexture(VkDevice device, VmaAllocator allocator, VkDescriptorPool descriptorPool, Texture &tex) {
  if (tex.descriptor && descriptorPool)
    vkFreeDescriptorSets(device, descriptorPool, 1, &tex.descriptor);
  if (tex.view && tex.ownsView)
    vkDestroyImageView(device, tex.view, nullptr);
  if (tex.image && tex.ownsImage)
    vmaDestroyImage(allocator, tex.image, tex.allocation);
  tex = {};
}

void destroySharedVulkanResources(SharedVulkanCore &core) {
  auto &res = core.resources;
  VkDevice const device = core.device;
  if (!device)
    return;
  destroySharedTexture(device, core.allocator, res.descriptorPool, res.atlas);
  if (res.pathPipeline)
    vkDestroyPipeline(device, res.pathPipeline, nullptr);
  if (res.rectPipeline)
    vkDestroyPipeline(device, res.rectPipeline, nullptr);
  if (res.calloutPipeline)
    vkDestroyPipeline(device, res.calloutPipeline, nullptr);
  if (res.imagePipeline)
    vkDestroyPipeline(device, res.imagePipeline, nullptr);
  if (res.imageUnpremultiplyPipeline)
    vkDestroyPipeline(device, res.imageUnpremultiplyPipeline, nullptr);
  if (res.backdropPipeline)
    vkDestroyPipeline(device, res.backdropPipeline, nullptr);
  if (res.backdropBlurPipeline)
    vkDestroyPipeline(device, res.backdropBlurPipeline, nullptr);
  if (res.pathPipelineLayout)
    vkDestroyPipelineLayout(device, res.pathPipelineLayout, nullptr);
  if (res.rectPipelineLayout)
    vkDestroyPipelineLayout(device, res.rectPipelineLayout, nullptr);
  if (res.calloutPipelineLayout)
    vkDestroyPipelineLayout(device, res.calloutPipelineLayout, nullptr);
  if (res.imagePipelineLayout)
    vkDestroyPipelineLayout(device, res.imagePipelineLayout, nullptr);
  if (res.backdropPipelineLayout)
    vkDestroyPipelineLayout(device, res.backdropPipelineLayout, nullptr);
  if (res.sampler)
    vkDestroySampler(device, res.sampler, nullptr);
  if (res.rectDescriptorLayout)
    vkDestroyDescriptorSetLayout(device, res.rectDescriptorLayout, nullptr);
  if (res.quadDescriptorLayout)
    vkDestroyDescriptorSetLayout(device, res.quadDescriptorLayout, nullptr);
  if (res.textureDescriptorLayout)
    vkDestroyDescriptorSetLayout(device, res.textureDescriptorLayout, nullptr);
  if (res.descriptorPool)
    vkDestroyDescriptorPool(device, res.descriptorPool, nullptr);
  saveAndDestroyPipelineCache(device, res);
  res = {};
}

CornerRadius clampRadii(CornerRadius r, float w, float h) {
  clampRoundRectCornerRadii(w, h, r);
  return r;
}

bool representativeFillColor(FillStyle const &fill, Color *out) {
  if (fill.solidColor(out))
    return true;
  LinearGradient linear{};
  if (fill.linearGradient(&linear) && linear.stopCount > 0) {
    *out = linear.stops[0].color;
    return true;
  }
  RadialGradient radial{};
  if (fill.radialGradient(&radial) && radial.stopCount > 0) {
    *out = radial.stops[0].color;
    return true;
  }
  ConicalGradient conical{};
  if (fill.conicalGradient(&conical) && conical.stopCount > 0) {
    *out = conical.stops[0].color;
    return true;
  }
  return false;
}

Rect boundsOfSubpaths(std::vector<std::vector<Point>> const &subpaths) {
  float minX = std::numeric_limits<float>::infinity();
  float minY = std::numeric_limits<float>::infinity();
  float maxX = -std::numeric_limits<float>::infinity();
  float maxY = -std::numeric_limits<float>::infinity();
  for (auto const &subpath : subpaths) {
    for (Point const &point : subpath) {
      minX = std::min(minX, point.x);
      minY = std::min(minY, point.y);
      maxX = std::max(maxX, point.x);
      maxY = std::max(maxY, point.y);
    }
  }
  if (!std::isfinite(minX) || maxX <= minX || maxY <= minY) {
    return Rect::sharp(0.f, 0.f, 1.f, 1.f);
  }
  return Rect::sharp(minX, minY, maxX - minX, maxY - minY);
}

void putPathGradient(VulkanPathVertex &out, FillStyle const &fill, Point local) {
  out.local[0] = local.x;
  out.local[1] = local.y;
  auto putStops = [&](auto const &gradient, int type) {
    out.params[0] = static_cast<float>(type);
    out.params[1] = static_cast<float>(gradient.stopCount);
    std::array<float *, 4> colors{out.fill0, out.fill1, out.fill2, out.fill3};
    for (std::uint8_t i = 0; i < gradient.stopCount && i < colors.size(); ++i) {
      putColor(colors[i], gradient.stops[i].color, 1.f);
      out.stops[i] = gradient.stops[i].position;
    }
  };
  LinearGradient linear{};
  if (fill.linearGradient(&linear) && linear.stopCount > 0) {
    putStops(linear, 1);
    out.gradient[0] = linear.start.x;
    out.gradient[1] = linear.start.y;
    out.gradient[2] = linear.end.x;
    out.gradient[3] = linear.end.y;
    return;
  }
  RadialGradient radial{};
  if (fill.radialGradient(&radial) && radial.stopCount > 0) {
    putStops(radial, 2);
    out.gradient[0] = radial.center.x;
    out.gradient[1] = radial.center.y;
    out.gradient[2] = radial.radius;
    return;
  }
  ConicalGradient conical{};
  if (fill.conicalGradient(&conical) && conical.stopCount > 0) {
    putStops(conical, 3);
    out.gradient[0] = conical.center.x;
    out.gradient[1] = conical.center.y;
    out.gradient[2] = conical.startAngleRadians;
  }
}

VulkanPathVertex makeVulkanPathVertex(PathVertex const &src, FillStyle const *fill, Rect bounds, float opacity) {
  VulkanPathVertex out{};
  out.x = src.x;
  out.y = src.y;
  std::copy(std::begin(src.color), std::end(src.color), std::begin(out.color));
  std::copy(std::begin(src.viewport), std::end(src.viewport), std::begin(out.viewport));
  float const invW = 1.f / std::max(bounds.width, 1e-4f);
  float const invH = 1.f / std::max(bounds.height, 1e-4f);
  Point const local{(src.x - bounds.x) * invW, (src.y - bounds.y) * invH};
  out.local[0] = local.x;
  out.local[1] = local.y;
  out.params[3] = opacity;
  if (fill) {
    putPathGradient(out, *fill, local);
  }
  return out;
}

} // namespace
namespace {

class VulkanCanvas final : public Canvas {
  friend bool ::lambdaui::deferVulkanFrameRecorderResourcesDestroy(
      VulkanFrameRecorderResources resources) noexcept;

public:
  VulkanCanvas(VkSurfaceKHR surface,
               unsigned int handle,
               TextSystem &textSystem,
      VulkanCanvasOptions options)
      : handle_(handle), textSystem_(textSystem), glyphAtlas_(textSystem), surface_(surface),
        transparentSurface_(options.transparentSurface) {
    instance_ = ensureSharedVulkanInstanceImpl();
    if (!surface_) {
      throw std::runtime_error("Vulkan canvas requires a valid platform surface");
    }
    SharedVulkanCore *shared = acquireSharedVulkanCore(surface_);
    ownsSharedVulkanCore_ = true;
    shared_ = shared;
    physical_ = shared->physical;
    device_ = shared->device;
    allocator_ = shared->allocator;
    queue_ = shared->queue;
    queueFamily_ = shared->queueFamily;
    if (shared->googleDisplayTiming) {
      getPastPresentationTiming_ = reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(
          vkGetDeviceProcAddr(device_, "vkGetPastPresentationTimingGOOGLE"));
    }
    createCommandObjects();
    chooseSurfaceFormat();
    ensureSharedResources();
    registerCanvas();
  }

  VulkanCanvas(VulkanRenderTargetSpec const &spec, TextSystem &textSystem)
      : textSystem_(textSystem), glyphAtlas_(textSystem), targetSpec_(spec), targetMode_(true) {
    instance_ = ensureSharedVulkanInstanceImpl();
    if (!targetSpec_.image || !targetSpec_.view || targetSpec_.width == 0 || targetSpec_.height == 0) {
      throw std::runtime_error("Vulkan RenderTarget requires image, view, width, and height");
    }
    SharedVulkanCore *shared = acquireSharedVulkanCore(VK_NULL_HANDLE);
    ownsSharedVulkanCore_ = true;
    shared_ = shared;
    physical_ = shared->physical;
    device_ = shared->device;
    allocator_ = shared->allocator;
    queue_ = shared->queue;
    queueFamily_ = shared->queueFamily;
    surfaceFormat_.format = targetSpec_.format == VK_FORMAT_UNDEFINED
                                 ? VK_FORMAT_B8G8R8A8_UNORM
                                 : targetSpec_.format;
    surfaceFormat_.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    width_ = static_cast<int>(targetSpec_.width);
    height_ = static_cast<int>(targetSpec_.height);
    framebufferWidth_ = width_;
    framebufferHeight_ = height_;
    swapExtent_ = VkExtent2D{targetSpec_.width, targetSpec_.height};
    createCommandObjects();
    ensureSharedResources();
    registerCanvas();
  }

  bool supportsDisplayTiming() const noexcept {
    return getPastPresentationTiming_ != nullptr && swapchain_ != VK_NULL_HANDLE;
  }

  bool usesMailboxPresentMode() const noexcept {
    return swapchain_ != VK_NULL_HANDLE && presentMode_ == VK_PRESENT_MODE_MAILBOX_KHR;
  }

  bool setImagePremultipliedAlpha(bool enabled) {
    bool const previous = imagePremultipliedAlpha_;
    imagePremultipliedAlpha_ = enabled;
    return previous;
  }

  bool setTransparentSurface(bool enabled) {
    if (targetMode_) {
      return false;
    }
    bool const changed = transparentSurface_ != enabled;
    transparentSurface_ = enabled;
    if (changed) {
      swapchainDirty_ = true;
    }
    return changed;
  }

  void setBackdropBlurBaseDownsample(std::uint32_t downsample) {
    std::uint32_t const next = std::clamp(downsample,
                                          kMinBackdropBlurBaseDownsample,
                                          kMaxBackdropBlurBaseDownsample);
    if (next == backdropBlurBaseDownsample_) {
      return;
    }
    backdropBlurBaseDownsample_ = next;
    backdropBlurCache_.clear();
    renderTargetFrameCacheValid_ = false;
  }

  bool setRenderTargetSpec(VulkanRenderTargetSpec const& spec) {
    if (!targetMode_ || !spec.image || !spec.view || spec.width == 0 || spec.height == 0) {
      return false;
    }
    targetSpec_ = spec;
    framebufferWidth_ = static_cast<int>(spec.width);
    framebufferHeight_ = static_cast<int>(spec.height);
    swapExtent_ = VkExtent2D{spec.width, spec.height};
    renderTargetFrameCacheValid_ = false;
    return true;
  }

  void markExternalRenderTargetSubmitted() {
    pendingFrameCaptureAwaitingExternalSubmit_ = false;
    if (frameCaptureTraceEnabled() && pendingFrameCaptureExternal_) {
      std::fprintf(stderr, "Lambda Vulkan: external render target submitted for frame capture\n");
    }
    retireDeferredResourcesAfterSubmit();
    renderTargetFrameCacheValid_ = false;
    currentFrame_ = (currentFrame_ + 1u) % kMaxFramesInFlight;
    debug::perf::recordPresentedFrame();
  }

  bool requestNextFrameCapture() override {
    frameCaptureRequested_ = true;
    return true;
  }

  bool takeCapturedFrame(std::vector<std::uint8_t>& out, std::uint32_t& width, std::uint32_t& height) override {
    pollFrameCapture(false);
    if (!capturedFrameAvailable_ || capturedFrameWidth_ == 0 || capturedFrameHeight_ == 0) {
      return false;
    }
    out = std::move(capturedFrameBytes_);
    width = capturedFrameWidth_;
    height = capturedFrameHeight_;
    capturedFrameBytes_.clear();
    capturedFrameWidth_ = 0;
    capturedFrameHeight_ = 0;
    capturedFrameAvailable_ = false;
    return true;
  }

  std::uint32_t lastPresentId() const noexcept {
    return lastSubmittedPresentId_;
  }

  std::vector<VulkanPastPresentationTiming> pollPastPresentationTimings() {
    if (!supportsDisplayTiming()) {
      return {};
    }
    std::uint32_t count = 0;
    VkResult result = getPastPresentationTiming_(device_, swapchain_, &count, nullptr);
    if (result != VK_SUCCESS || count == 0) {
      return {};
    }
    std::vector<VkPastPresentationTimingGOOGLE> timings(count);
    result = getPastPresentationTiming_(device_, swapchain_, &count, timings.data());
    if (result != VK_SUCCESS) {
      return {};
    }
    timings.resize(count);
    std::vector<VulkanPastPresentationTiming> out;
    out.reserve(timings.size());
    for (auto const& timing : timings) {
      out.push_back(VulkanPastPresentationTiming{
          .presentId = timing.presentID,
          .desiredPresentTime = timing.desiredPresentTime,
          .actualPresentTime = timing.actualPresentTime,
          .earliestPresentTime = timing.earliestPresentTime,
          .presentMargin = timing.presentMargin,
      });
    }
    return out;
  }

  bool recordedOpsGlyphAtlasCurrent(VulkanFrameRecorder const& recorded) const {
    return recordedGlyphAtlasCurrent(recorded);
  }

  ~VulkanCanvas() override {
    if (device_) {
      if (deviceWaitIdleOnDestruct_) {
        // Intentional canvas teardown wait before destroying swapchain/present resources.
        vkDeviceWaitIdle(device_);
      } else {
        waitForSubmittedFrameWork();
      }
      pollReadbacks(true);
    }
    unregisterCanvas();
    destroySwapchain();
    destroyPendingTextureUploads();
    destroyTexture(backdropSceneTexture_);
    destroyTexture(backdropScratchTexture_);
    destroyTexture(backdropBlurTexture_);
    clearBackdropBlurCache();
    for (auto &kv : imageTextures_) {
      if (kv.second) {
        destroyTexture(*kv.second);
      }
    }
    destroyDeferredTextures(true);
    destroyDeferredBuffers(true);
    destroyDeferredRecorderResources(true);
    destroyDeferredOwnedImages(true);
    for (Buffer& buffer : uploadStagingPool_) {
      destroyBuffer(buffer);
    }
    uploadStagingPool_.clear();
    destroyBuffer(pendingScreenshotBuffer_);
    destroyBuffer(pendingFrameCaptureBuffer_);
    for (auto& geometry : frameGeometry_) {
      destroyFrameGeometryResources(geometry);
    }
    for (VkSemaphore semaphore : imageAvailable_) {
      if (semaphore)
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    for (VkSemaphore semaphore : imageRenderFinished_) {
      if (semaphore)
        vkDestroySemaphore(device_, semaphore, nullptr);
    }
    for (VkFence fence : frameFences_) {
      if (fence)
        vkDestroyFence(device_, fence, nullptr);
    }
    if (commandPool_)
      vkDestroyCommandPool(device_, commandPool_, nullptr);
    if (surface_)
      vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (ownsSharedVulkanCore_)
      releaseSharedVulkanCore();
  }

  void waitForSubmittedFrameWork() {
    if (!device_) {
      return;
    }
    for (std::size_t i = 0; i < frameFences_.size(); ++i) {
      VkFence const fence = frameFences_[i];
      if (fence == VK_NULL_HANDLE ||
          frameFenceCompleteGenerations_[i] >= frameFenceSubmitGenerations_[i]) {
        continue;
      }
      vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
      markFrameFenceComplete(i);
    }
  }

  Backend backend() const noexcept override { return Backend::Vulkan; }
  unsigned int windowHandle() const override { return handle_; }

  void resize(int width, int height) override {
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    int const fbW = std::max(1, static_cast<int>(std::lround(static_cast<float>(width_) * dpiScaleX_)));
    int const fbH = std::max(1, static_cast<int>(std::lround(static_cast<float>(height_) * dpiScaleY_)));
    bool const framebufferChanged = fbW != framebufferWidth_ || fbH != framebufferHeight_;
    framebufferWidth_ = fbW;
    framebufferHeight_ = fbH;
    if (targetMode_) {
      return;
    }
    bool const needsLargerSwapchain =
        !swapchain_ || swapExtent_.width == 0 || swapExtent_.height == 0 ||
        fbW > static_cast<int>(swapExtent_.width) || fbH > static_cast<int>(swapExtent_.height);
    if (needsLargerSwapchain) {
      bool const addHeadroom = swapchain_ != VK_NULL_HANDLE;
      swapchainTargetWidth_ = fbW + (addHeadroom ? std::max(512, fbW / 2) : 0);
      swapchainTargetHeight_ = fbH + (addHeadroom ? std::max(512, fbH / 2) : 0);
      swapchainDirty_ = true;
      if (detail::resizeTraceEnabled()) {
        LAMBDA_RESIZE_TRACE(
          "vulkan-resize",
          "window=%u logical=%dx%d framebuffer=%dx%d target=%dx%d boundsHint=%dx%d dirty=1\n",
                     handle_, width_, height_, framebufferWidth_, framebufferHeight_,
                     swapchainTargetWidth_, swapchainTargetHeight_,
                     resizeBoundsHintWidth_,
                     resizeBoundsHintHeight_);
      }
    } else if (framebufferChanged && detail::resizeTraceEnabled()) {
      LAMBDA_RESIZE_TRACE(
        "vulkan-resize",
        "window=%u logical=%dx%d framebuffer=%dx%d extent=%ux%u dirty=0\n",
                   handle_, width_, height_, framebufferWidth_, framebufferHeight_,
                   swapExtent_.width, swapExtent_.height);
    }
  }

  void updateDpiScale(float sx, float sy) override {
    dpiScaleX_ = std::max(0.25f, sx);
    dpiScaleY_ = std::max(0.25f, sy);
    resize(width_, height_);
  }

  float dpiScale() const noexcept override { return std::max(dpiScaleX_, dpiScaleY_); }

  static VulkanFrameRecorder* asVulkanRecordedOps(RecordedOps* recorded) noexcept {
    return recorded && recorded->backend() == Backend::Vulkan
               ? static_cast<VulkanFrameRecorder*>(recorded)
               : nullptr;
  }

  static VulkanFrameRecorder const* asVulkanRecordedOps(RecordedOps const& recorded) noexcept {
    return recorded.backend() == Backend::Vulkan
               ? static_cast<VulkanFrameRecorder const*>(&recorded)
               : nullptr;
  }

  static bool sameClipRect(Rect a, Rect b) noexcept {
    constexpr float eps = 1e-4f;
    return std::abs(a.x - b.x) <= eps &&
           std::abs(a.y - b.y) <= eps &&
           std::abs(a.width - b.width) <= eps &&
           std::abs(a.height - b.height) <= eps;
  }

  static bool recordedOpsContainClipState(VulkanFrameRecorder const& recorded) noexcept {
    return std::any_of(recorded.ops.begin(), recorded.ops.end(), [&recorded](DrawOp const& op) {
      return !sameClipRect(op.clip, recorded.rootClip);
    });
  }

  class VulkanCanvasPreparedRenderOps final : public scenegraph::PreparedRenderOps {
  public:
    explicit VulkanCanvasPreparedRenderOps(VulkanFrameRecorder recorded)
        : recorded_(std::move(recorded)) {}

    bool replay(Canvas& canvas) const override {
      atlasMismatchOnLastReplay_ = false;
      VulkanCanvas* vulkanCanvas = dynamic_cast<VulkanCanvas*>(&canvas);
      if (recorded_.glyphAtlasGeneration != 0 &&
          (!vulkanCanvas || !vulkanCanvas->recordedOpsGlyphAtlasCurrent(recorded_))) {
        atlasMismatchOnLastReplay_ = true;
        return false;
      }
      return canvas.replayRecordedLocalOps(recorded_);
    }

    bool reusableAfterReplayFailure() const override {
      return !atlasMismatchOnLastReplay_;
    }

  private:
    VulkanFrameRecorder recorded_;
    mutable bool atlasMismatchOnLastReplay_ = false;
  };

  class CanvasUnreplayablePreparedRenderOps final : public scenegraph::PreparedRenderOps {
  public:
    bool replay(Canvas&) const override {
      return false;
    }
  };

  void setResizeBoundsHint(int logicalWidth, int logicalHeight) {
    resizeBoundsHintWidth_ = std::max(0, logicalWidth);
    resizeBoundsHintHeight_ = std::max(0, logicalHeight);
  }

  std::unique_ptr<RecordedOps> beginRecordedOpsCapture() override {
    if (captureTarget_) {
      return nullptr;
    }
    auto recorded = std::make_unique<VulkanFrameRecorder>();
    beginRecordedOpsCapture(recorded.get());
    return recorded;
  }

  void beginRecordedOpsCapture(VulkanFrameRecorder *target) {
    if (!target || captureTarget_) {
      return;
    }
    target->clear();
    target->rootClip = Rect::sharp(0.f, 0.f, static_cast<float>(width_), static_cast<float>(height_));
    captureSavedState_ = state_;
    captureSavedStack_ = stateStack_;
    hasCaptureSavedState_ = true;
    captureTarget_ = target;
    stateStack_.clear();
    state_ = {};
    state_.clip = target->rootClip;
  }

  void endRecordedOpsCapture() override {
    captureTarget_ = nullptr;
    if (hasCaptureSavedState_) {
      state_ = captureSavedState_;
      stateStack_ = std::move(captureSavedStack_);
      captureSavedState_ = {};
      captureSavedStack_.clear();
      hasCaptureSavedState_ = false;
    }
  }

  std::unique_ptr<scenegraph::PreparedRenderOps> finalizeRecordedOps(
      std::unique_ptr<RecordedOps> recorded) override {
    VulkanFrameRecorder* vulkanRecorded = asVulkanRecordedOps(recorded.get());
    if (!vulkanRecorded) {
      return nullptr;
    }
    if (!prepareRecorderForReplay(vulkanRecorded)) {
      return std::make_unique<CanvasUnreplayablePreparedRenderOps>();
    }
    if (recordedOpsContainClipState(*vulkanRecorded)) {
      return std::make_unique<CanvasUnreplayablePreparedRenderOps>();
    }
    return std::make_unique<VulkanCanvasPreparedRenderOps>(std::move(*vulkanRecorded));
  }

  bool replayRecordedOps(RecordedOps const& recorded,
                         RecordedOpsReplaySlice const* slice = nullptr) override {
    if (slice && slice->backend != Backend::Vulkan) {
      return false;
    }
    VulkanFrameRecorder const* vulkanRecorded = asVulkanRecordedOps(recorded);
    return vulkanRecorded && replayRecordedOps(*vulkanRecorded);
  }

  bool replayRecordedOps(VulkanFrameRecorder const &recorded) {
    if (!recordedGlyphAtlasCurrent(recorded)) {
      return false;
    }
    if (!prepareRecorderBuffers(recorded)) {
      return false;
    }
    return appendRecordedOps(recorded, false);
  }

  bool replayRecordedLocalOps(RecordedOps const& recorded,
                              RecordedOpsReplaySlice const* slice = nullptr) override {
    if (slice && slice->backend != Backend::Vulkan) {
      return false;
    }
    VulkanFrameRecorder const* vulkanRecorded = asVulkanRecordedOps(recorded);
    return vulkanRecorded && replayRecordedLocalOps(*vulkanRecorded);
  }

  bool replayRecordedLocalOps(VulkanFrameRecorder const &recorded) {
    if (!recordedGlyphAtlasCurrent(recorded) || !state_.transform.isTranslationOnly()) {
      return false;
    }
    if (!prepareRecorderBuffers(recorded)) {
      return false;
    }
    return appendRecordedOps(recorded, true);
  }

  bool prepareRecorderForReplay(VulkanFrameRecorder *recorded) {
    if (!recorded) {
      return false;
    }
    if (!recorded->replayable) {
      return false;
    }
    prepareRecordedGeometrySignatures(*recorded);
    return prepareRecorderBuffers(*recorded);
  }

  void beginFrame() override {
    captureTarget_ = nullptr;
    hasCaptureSavedState_ = false;
    captureSavedStack_.clear();
    rects_.clear();
    callouts_.clear();
    quads_.clear();
    batches_.clear();
    ops_.clear();
    pathVerts_.clear();
    stateStack_.clear();
    state_ = {};
    state_.clip = Rect::sharp(0.f, 0.f, static_cast<float>(width_), static_cast<float>(height_));
  }

  void clear(Color color = Colors::transparent) override { clearColor_ = color; }

  std::shared_ptr<Image> rasterize(Size logicalSize, RasterizeDrawCallback const &draw, float dpiScale) {
    if (!draw || logicalSize.width <= 0.f || logicalSize.height <= 0.f)
      return nullptr;
    float const scale = dpiScale > 0.f ? dpiScale : std::max(dpiScaleX_, dpiScaleY_);
    int const logicalW = std::max(1, static_cast<int>(std::ceil(logicalSize.width)));
    int const logicalH = std::max(1, static_cast<int>(std::ceil(logicalSize.height)));
    int const pixelW = std::max(1, static_cast<int>(std::ceil(logicalSize.width * scale)));
    int const pixelH = std::max(1, static_cast<int>(std::ceil(logicalSize.height * scale)));

    Texture target{};
    try {
      createRenderTargetTexture(target, pixelW, pixelH);
      VulkanRenderTargetSpec spec{
          .image = target.image,
          .view = target.view,
          .format = target.format,
          .width = static_cast<std::uint32_t>(pixelW),
          .height = static_cast<std::uint32_t>(pixelH),
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .finalLayout = VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL,
      };
      {
        VulkanCanvas targetCanvas(spec, textSystem_);
        targetCanvas.resize(logicalW, logicalH);
        targetCanvas.updateDpiScale(scale, scale);
        targetCanvas.beginFrame();
        targetCanvas.clear(Colors::transparent);
        draw(targetCanvas, Rect::sharp(0.f, 0.f, logicalSize.width, logicalSize.height));
        targetCanvas.present();
        targetCanvas.waitForSubmittedFrameWork();
        targetCanvas.deviceWaitIdleOnDestruct_ = false;
      }

      auto image = std::make_shared<VulkanImage>(device_, allocator_, target.image, target.allocation,
                                                 target.view, target.format, pixelW, pixelH, true);
      target.image = VK_NULL_HANDLE;
      target.allocation = VK_NULL_HANDLE;
      target.view = VK_NULL_HANDLE;
      return image;
    } catch (...) {
      destroyTexture(target);
      throw;
    }
  }

  void present() override {
    if (width_ <= 0 || height_ <= 0 || framebufferWidth_ <= 0 || framebufferHeight_ <= 0)
      return;
    debug::perf::ScopedTimer timer(debug::perf::TimedMetric::CanvasPresent);
    if (targetMode_) {
      presentRenderTarget();
      return;
    }
    try {
      auto const presentStart = std::chrono::steady_clock::now();
      if (swapchainDirty_ || !swapchain_) {
        recreateSwapchain();
      }
      if (!swapchain_)
        return;
      presentImpl();
      if (detail::resizeTraceEnabled()) {
        auto const elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - presentStart).count();
        LAMBDA_RESIZE_TRACE(
          "vulkan-present",
          "window=%u logical=%dx%d framebuffer=%dx%d extent=%ux%u elapsed=%.3fms\n",
                     handle_, width_, height_, framebufferWidth_, framebufferHeight_,
                     swapExtent_.width, swapExtent_.height,
                     static_cast<double>(elapsed) / 1000.0);
      }
    } catch (std::exception const &e) {
      recoverResetFrameFence();
      std::fprintf(stderr, "Lambda Vulkan: present failed: %s\n", e.what());
      swapchainDirty_ = true;
    }
  }

  void presentImpl() {
    bool const traceResize = detail::resizeTraceEnabled();
    auto const phaseStart = [&] {
      return traceResize ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    };
    auto const phaseMs = [&](std::chrono::steady_clock::time_point start) {
      if (!traceResize) return 0.0;
      auto const elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start).count();
      return static_cast<double>(elapsed) / 1000.0;
    };

    VkFence const frameFence = frameFences_[currentFrame_];
    VkSemaphore const imageAvailable = imageAvailable_[currentFrame_];
    VkCommandBuffer const commandBuffer = commandBuffers_[currentFrame_];

    auto start = phaseStart();
    vkWaitForFences(device_, 1, &frameFence, VK_TRUE, UINT64_MAX);
    markFrameFenceComplete(currentFrame_);
    double const frameFenceMs = phaseMs(start);
    start = phaseStart();
    pollReadbacks(false);
    double screenshotMs = phaseMs(start);
    for (VkFence& imageFence : imageInFlightFences_) {
      if (imageFence == frameFence) imageFence = VK_NULL_HANDLE;
    }
    double deferredDestroyMs = 0.0;
    if (!swapchainImageStateReadyForAcquire()) {
      swapchainDirty_ = true;
      return;
    }
    std::uint32_t imageIndex = 0;
    start = phaseStart();
    VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, imageAvailable, VK_NULL_HANDLE,
                                              &imageIndex);
    double const acquireMs = phaseMs(start);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
      swapchainDirty_ = true;
      return;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
      vkCheck(acquired, "vkAcquireNextImageKHR");
    }
    assert(imageIndex < imageInFlightFences_.size());
    assert(imageIndex < imageRenderFinished_.size());
    assert(imageRenderFinished_[imageIndex] != VK_NULL_HANDLE);
    double imageFenceMs = 0.0;
    if (imageInFlightFences_[imageIndex]) {
      start = phaseStart();
      vkWaitForFences(device_, 1, &imageInFlightFences_[imageIndex], VK_TRUE, UINT64_MAX);
      markFrameFenceComplete(imageInFlightFences_[imageIndex]);
      imageFenceMs = phaseMs(start);
    }
    imageInFlightFences_[imageIndex] = frameFence;
    VkSemaphore const renderFinished = imageRenderFinished_[imageIndex];

    start = phaseStart();
    queueAtlasUploadIfNeeded();
    double const atlasMs = phaseMs(start);
    start = phaseStart();
    auto backdropRuns = prepareBackdropBlurRuns();
    double const backdropPrepareMs = phaseMs(start);
    start = phaseStart();
    uploadFrameBuffers();
    double const uploadMs = phaseMs(start);

    start = phaseStart();
    vkResetCommandBuffer(commandBuffer, 0);
    auto begin = vkStructure<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    vkCheck(vkBeginCommandBuffer(commandBuffer, &begin), "vkBeginCommandBuffer");
    VkClearValue clear{};
    clear.color.float32[0] = clearColor_.r;
    clear.color.float32[1] = clearColor_.g;
    clear.color.float32[2] = clearColor_.b;
    clear.color.float32[3] = clearColor_.a;
    recordPendingTextureTransitions(commandBuffer);
    recordPendingTextureUploads(commandBuffer);
    if (!backdropRuns.empty() && backdropSceneTexture_.view && backdropScratchTexture_.view && backdropBlurTexture_.view) {
      VkImageLayout targetLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      drawOpsWithStackedBackdropBlur(commandBuffer,
                                     swapchainImages_[imageIndex],
                                     swapchainViews_[imageIndex],
                                     targetLayout,
                                     clear,
                                     VK_ATTACHMENT_LOAD_OP_CLEAR,
                                     backdropRuns);
    } else {
      transition(commandBuffer, swapchainImages_[imageIndex], VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL);
      beginColorRendering(commandBuffer,
                          swapchainViews_[imageIndex],
                          swapExtent_,
                          clear,
                          VK_ATTACHMENT_LOAD_OP_CLEAR,
                          currentFramebufferPixelRect());
      drawOps(commandBuffer);
      vkCmdEndRendering(commandBuffer);
    }
    transition(commandBuffer, swapchainImages_[imageIndex], VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    writeDebugScreenshotIfRequested(commandBuffer, swapchainImages_[imageIndex], frameFence);
    captureFrameIfRequested(commandBuffer, swapchainImages_[imageIndex], frameFence);
    transition(commandBuffer, swapchainImages_[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkCheck(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");
    double const recordMs = phaseMs(start);

    auto waitSemaphore = vkStructure<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
    waitSemaphore.semaphore = imageAvailable;
    waitSemaphore.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    auto commandBufferInfo = vkStructure<VkCommandBufferSubmitInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
    commandBufferInfo.commandBuffer = commandBuffer;
    auto signalSemaphore = vkStructure<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
    signalSemaphore.semaphore = renderFinished;
    signalSemaphore.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
    auto submit = vkStructure<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &waitSemaphore;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &commandBufferInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signalSemaphore;
    resetFrameFenceIndex_ = currentFrame_;
    vkCheck(vkResetFences(device_, 1, &frameFence), "vkResetFences");
    start = phaseStart();
    VkResult const submitted = vkQueueSubmit2(queue_, 1, &submit, frameFence);
    double const submitMs = phaseMs(start);
    if (submitted != VK_SUCCESS) {
      recoverResetFrameFence();
      vkCheck(submitted, "vkQueueSubmit");
    }
    markFrameFenceSubmitted(currentFrame_);
    resetFrameFenceIndex_ = kNoResetFrameFence;
    start = phaseStart();
    retireDeferredResourcesAfterSubmit();
    deferredDestroyMs = phaseMs(start);
    start = phaseStart();
    pollReadbacks(false);
    screenshotMs += phaseMs(start);

    auto presentInfo = vkStructure<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &renderFinished;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;
    VkFence presentFence = VK_NULL_HANDLE;
    auto presentFenceInfo =
        vkStructure<VkSwapchainPresentFenceInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR);
    double presentFenceStatusMs = 0.0;
    double presentFenceResetMs = 0.0;
    bool const usePresentFence = shared_ && shared_->swapchainMaintenance1 && !presentFenceRuntimeDisabled_ &&
                                 imageIndex < imagePresentFences_.size() &&
                                 imagePresentFences_[imageIndex] != VK_NULL_HANDLE;
    if (usePresentFence) {
      presentFence = imagePresentFences_[imageIndex];
      start = phaseStart();
      VkResult const fenceStatus = vkGetFenceStatus(device_, presentFence);
      presentFenceStatusMs = phaseMs(start);
      if (fenceStatus == VK_SUCCESS) {
        start = phaseStart();
        vkCheck(vkResetFences(device_, 1, &presentFence), "vkResetFences(presentFence)");
        presentFenceResetMs = phaseMs(start);
      } else if (fenceStatus == VK_NOT_READY) {
        VkFence replacement = createPresentFence(false, "vkCreateFence(presentFence replacement)");
        retirePresentFence(presentFence);
        imagePresentFences_[imageIndex] = replacement;
        presentFence = replacement;
      } else {
        vkCheck(fenceStatus, "vkGetFenceStatus(presentFence)");
      }
      presentFenceInfo.swapchainCount = 1;
      presentFenceInfo.pFences = &presentFence;
    }
    VkPresentTimeGOOGLE presentTime{.presentID = nextPresentId_, .desiredPresentTime = 0};
    auto presentTimes = vkStructure<VkPresentTimesInfoGOOGLE>(VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE);
    if (getPastPresentationTiming_) {
      presentTimes.swapchainCount = 1;
      presentTimes.pTimes = &presentTime;
      presentInfo.pNext = &presentTimes;
    }
    if (presentFence) {
      presentFenceInfo.pNext = presentInfo.pNext;
      presentInfo.pNext = &presentFenceInfo;
    }
    bool const presentFenceQueued = presentFence != VK_NULL_HANDLE;
    start = phaseStart();
    VkResult presented = vkQueuePresentKHR(queue_, &presentInfo);
    double const presentQueueMs = phaseMs(start);
    bool const presentAccepted = presented == VK_SUCCESS || presented == VK_SUBOPTIMAL_KHR;
    if (!presentAccepted && presentFence && imageIndex < imagePresentFences_.size() &&
        imagePresentFences_[imageIndex] == presentFence) {
      retirePresentFence(presentFence);
      imagePresentFences_[imageIndex] = createPresentFence(true, "vkCreateFence(presentFence after failed present)");
      presentFence = VK_NULL_HANDLE;
    }
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
      swapchainDirty_ = true;
    } else {
      vkCheck(presented, "vkQueuePresentKHR");
    }
    if (getPastPresentationTiming_ && (presented == VK_SUCCESS || presented == VK_SUBOPTIMAL_KHR)) {
      lastSubmittedPresentId_ = presentTime.presentID;
      ++nextPresentId_;
    } else {
      lastSubmittedPresentId_ = 0;
    }
    currentFrame_ = (currentFrame_ + 1u) % kMaxFramesInFlight;
    debug::perf::recordPresentedFrame();
    if (traceResize) {
      LAMBDA_RESIZE_TRACE(
          "vulkan-present-detail",
          "window=%u image=%u ops=%zu rects=%zu quads=%zu paths=%zu "
          "waitFrame=%.3fms deferred=%.3fms acquire=%.3fms waitImage=%.3fms "
          "atlas=%.3fms blurPrep=%.3fms upload=%.3fms record=%.3fms submit=%.3fms "
          "screenshot=%.3fms presentFence=%d statusPresentFence=%.3fms resetPresentFence=%.3fms "
          "queuePresent=%.3fms\n",
          handle_,
          imageIndex,
          ops_.size(),
          rects_.size(),
          quads_.size(),
          pathVerts_.size(),
          frameFenceMs,
          deferredDestroyMs,
          acquireMs,
          imageFenceMs,
          atlasMs,
          backdropPrepareMs,
          uploadMs,
          recordMs,
          submitMs,
          screenshotMs,
          presentFenceQueued ? 1 : 0,
          presentFenceStatusMs,
          presentFenceResetMs,
          presentQueueMs);
    }
  }

  struct RenderTargetRecordStats {
    double atlasMs = 0.0;
    double backdropPrepareMs = 0.0;
    double uploadMs = 0.0;
    double textureUploadRecordMs = 0.0;
    double drawRecordMs = 0.0;
    std::size_t backdropRuns = 0;
  };

  void recordRenderTargetCommands(VkCommandBuffer commandBuffer, VkImage targetImage,
                                  VkImageView targetView, VkImageLayout initialLayout,
                                  VkImageLayout finalLayout,
                                  RenderTargetRecordStats* stats = nullptr) {
    auto const recordStart = std::chrono::steady_clock::now();
    bool const traceResize = stats && detail::resizeTraceEnabled();
    auto const phaseStart = [&] {
      return traceResize ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    };
    auto const phaseMs = [&](std::chrono::steady_clock::time_point start) {
      if (!traceResize) return 0.0;
      auto const elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - start).count();
      return static_cast<double>(elapsed) / 1000.0;
    };

    auto start = phaseStart();
    queueAtlasUploadIfNeeded();
    if (stats) stats->atlasMs = phaseMs(start);
    start = phaseStart();
    std::vector<BackdropBlurRun> backdropRuns = prepareBackdropBlurRuns();
    if (stats) {
      stats->backdropPrepareMs = phaseMs(start);
      stats->backdropRuns = backdropRuns.size();
    }
    start = phaseStart();
    uploadFrameBuffers();
    if (stats) stats->uploadMs = phaseMs(start);
    start = phaseStart();
    recordPendingTextureTransitions(commandBuffer);
    recordPendingTextureUploads(commandBuffer);
    if (stats) stats->textureUploadRecordMs = phaseMs(start);

    VkClearValue clear{};
    clear.color.float32[0] = clearColor_.r;
    clear.color.float32[1] = clearColor_.g;
    clear.color.float32[2] = clearColor_.b;
    clear.color.float32[3] = clearColor_.a;
    VkImageLayout targetLayout = initialLayout;
    start = phaseStart();
    VkAttachmentLoadOp const loadOp =
        targetSpec_.preserveContents ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
    if (!backdropRuns.empty() && backdropSceneTexture_.view && backdropScratchTexture_.view && backdropBlurTexture_.view) {
      drawOpsWithStackedBackdropBlur(commandBuffer,
                                     targetImage,
                                     targetView,
                                     targetLayout,
                                     clear,
                                     loadOp,
                                     backdropRuns);
    } else {
      transition(commandBuffer, targetImage, targetLayout, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL);
      targetLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
      beginColorRendering(commandBuffer, targetView, swapExtent_, clear, loadOp);
      drawOps(commandBuffer);
      vkCmdEndRendering(commandBuffer);
    }
    if (stats) stats->drawRecordMs = phaseMs(start);
    transition(commandBuffer, targetImage, targetLayout, finalLayout);
    debug::perf::recordVulkanRecordDuration(std::chrono::steady_clock::now() - recordStart);
  }

  void presentRenderTarget() {
    try {
      targetSpec_.width = std::max<std::uint32_t>(1, targetSpec_.width);
      targetSpec_.height = std::max<std::uint32_t>(1, targetSpec_.height);
      framebufferWidth_ = static_cast<int>(targetSpec_.width);
      framebufferHeight_ = static_cast<int>(targetSpec_.height);
      swapExtent_ = VkExtent2D{targetSpec_.width, targetSpec_.height};

      bool const externalCommandBuffer = targetSpec_.commandBuffer != VK_NULL_HANDLE;
      bool const canReuseCompletedFrame =
          targetMode_ &&
          !externalCommandBuffer &&
          !targetSpec_.waitSemaphore &&
          !targetSpec_.signalSemaphore &&
          !frameCaptureRequested_ &&
          !renderTargetFrameCacheDisabled() &&
          !resources().atlasDirty;
      std::uint64_t const requestedFrameSignature =
          canReuseCompletedFrame ? renderTargetFrameSignature() : 0;
      if (canReuseCompletedFrame &&
          renderTargetFrameCacheValid_ &&
          renderTargetFrameSignature_ == requestedFrameSignature) {
        debug::perf::recordPresentedFrame();
        return;
      }
      VkCommandBuffer commandBuffer = targetSpec_.commandBuffer;
      VkFence frameFence = externalCommandBuffer ? targetSpec_.completionFence : VK_NULL_HANDLE;
      bool const traceResize = detail::resizeTraceEnabled();
      auto const phaseStart = [&] {
        return traceResize ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      };
      auto const phaseMs = [&](std::chrono::steady_clock::time_point start) {
        if (!traceResize) return 0.0;
        auto const elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
        return static_cast<double>(elapsed) / 1000.0;
      };
      double waitFrameMs = 0.0;
      double deferredDestroyMs = 0.0;
      double beginMs = 0.0;
      double endMs = 0.0;
      double submitMs = 0.0;
      if (!externalCommandBuffer) {
        frameFence = frameFences_[currentFrame_];
        commandBuffer = commandBuffers_[currentFrame_];
        auto start = phaseStart();
        vkWaitForFences(device_, 1, &frameFence, VK_TRUE, UINT64_MAX);
        markFrameFenceComplete(currentFrame_);
        waitFrameMs = phaseMs(start);
        pollReadbacks(false);
        start = phaseStart();
        vkResetCommandBuffer(commandBuffer, 0);
        auto begin = vkStructure<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkCheck(vkBeginCommandBuffer(commandBuffer, &begin), "vkBeginCommandBuffer");
        beginMs = phaseMs(start);
      }

      RenderTargetRecordStats recordStats{};
      recordRenderTargetCommands(commandBuffer, targetSpec_.image, targetSpec_.view,
                                 targetSpec_.initialLayout, targetSpec_.finalLayout,
                                 traceResize ? &recordStats : nullptr);
      if (frameCaptureRequested_) {
        transition(commandBuffer, targetSpec_.image, targetSpec_.finalLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        captureFrameIfRequested(commandBuffer, targetSpec_.image, frameFence, externalCommandBuffer);
        transition(commandBuffer, targetSpec_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, targetSpec_.finalLayout);
      }

      if (externalCommandBuffer) {
        return;
      }

      auto start = phaseStart();
      vkCheck(vkEndCommandBuffer(commandBuffer), "vkEndCommandBuffer");
      endMs = phaseMs(start);

      auto waitSemaphore = vkStructure<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
      waitSemaphore.semaphore = targetSpec_.waitSemaphore;
      waitSemaphore.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      auto commandBufferInfo = vkStructure<VkCommandBufferSubmitInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO);
      commandBufferInfo.commandBuffer = commandBuffer;
      auto signalSemaphore = vkStructure<VkSemaphoreSubmitInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO);
      signalSemaphore.semaphore = targetSpec_.signalSemaphore;
      signalSemaphore.stageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT;
      auto submit = vkStructure<VkSubmitInfo2>(VK_STRUCTURE_TYPE_SUBMIT_INFO_2);
      submit.waitSemaphoreInfoCount = targetSpec_.waitSemaphore ? 1u : 0u;
      submit.pWaitSemaphoreInfos = targetSpec_.waitSemaphore ? &waitSemaphore : nullptr;
      submit.commandBufferInfoCount = 1;
      submit.pCommandBufferInfos = &commandBufferInfo;
      submit.signalSemaphoreInfoCount = targetSpec_.signalSemaphore ? 1u : 0u;
      submit.pSignalSemaphoreInfos = targetSpec_.signalSemaphore ? &signalSemaphore : nullptr;
      resetFrameFenceIndex_ = currentFrame_;
      vkCheck(vkResetFences(device_, 1, &frameFence), "vkResetFences");
      start = phaseStart();
      VkResult const submitted = vkQueueSubmit2(queue_, 1, &submit, frameFence);
      submitMs = phaseMs(start);
      if (submitted != VK_SUCCESS) {
        recoverResetFrameFence();
        vkCheck(submitted, "vkQueueSubmit2");
      }
      markFrameFenceSubmitted(currentFrame_);
      resetFrameFenceIndex_ = kNoResetFrameFence;
      start = phaseStart();
      retireDeferredResourcesAfterSubmit();
      deferredDestroyMs = phaseMs(start);
      if (!targetSpec_.signalSemaphore) {
        // Render-target canvases without an exported signal are synchronous by contract;
        // callers use them for offscreen rasterization and immediate readback.
        vkWaitForFences(device_, 1, &frameFence, VK_TRUE, UINT64_MAX);
        markFrameFenceComplete(currentFrame_);
      }
      pollFrameCapture(false);
      if (canReuseCompletedFrame) {
        renderTargetFrameSignature_ = renderTargetFrameSignature();
        renderTargetFrameCacheValid_ = true;
      } else {
        renderTargetFrameCacheValid_ = false;
      }
      currentFrame_ = (currentFrame_ + 1u) % kMaxFramesInFlight;
      debug::perf::recordPresentedFrame();
      if (traceResize) {
        LAMBDA_RESIZE_TRACE(
            "vulkan-render-target-detail",
            "window=%u target=%ux%u ops=%zu rects=%zu quads=%zu paths=%zu backdropRuns=%zu "
            "waitFrame=%.3fms deferred=%.3fms begin=%.3fms atlas=%.3fms blurPrep=%.3fms "
            "upload=%.3fms textureUploadRecord=%.3fms drawRecord=%.3fms end=%.3fms submit=%.3fms "
            "signal=%d\n",
            handle_,
            targetSpec_.width,
            targetSpec_.height,
            ops_.size(),
            rects_.size(),
            quads_.size(),
            pathVerts_.size(),
            recordStats.backdropRuns,
            waitFrameMs,
            deferredDestroyMs,
            beginMs,
            recordStats.atlasMs,
            recordStats.backdropPrepareMs,
            recordStats.uploadMs,
            recordStats.textureUploadRecordMs,
            recordStats.drawRecordMs,
            endMs,
            submitMs,
            targetSpec_.signalSemaphore ? 1 : 0);
      }
    } catch (std::exception const &e) {
      recoverResetFrameFence();
      renderTargetFrameCacheValid_ = false;
      std::fprintf(stderr, "Lambda Vulkan: render target present failed: %s\n", e.what());
    }
  }

  void save() override { stateStack_.push_back(state_); }
  void restore() override {
    if (!stateStack_.empty()) {
      state_ = stateStack_.back();
      stateStack_.pop_back();
    }
  }
  void setTransform(Mat3 const &m) override { state_.transform = m; }
  void transform(Mat3 const &m) override { state_.transform = state_.transform * m; }
  void translate(Point p) override { transform(Mat3::translate(p)); }
  void translate(float x, float y) override { translate({x, y}); }
  void scale(float sx, float sy) override { transform(Mat3::scale(sx, sy)); }
  void scale(float s) override { scale(s, s); }
  void rotate(float r) override { transform(Mat3::rotate(r)); }
  void rotate(float r, Point p) override { transform(Mat3::rotate(r, p)); }
  Mat3 currentTransform() const override { return state_.transform; }

  void clipRect(Rect rect, CornerRadius const &cornerRadius, bool) override {
    Rect r = transformedBounds(rect);
    Rect const effective = lambdaui::intersectRects(state_.clip, r);
    state_.clip = effective;
    if (!cornerRadius.isZero() && r.width > 0.f && r.height > 0.f) {
      float const sx = std::hypot(state_.transform.m[0], state_.transform.m[1]);
      float const sy = std::hypot(state_.transform.m[3], state_.transform.m[4]);
      float const radiusScale = std::max(0.f, std::min(sx, sy));
      CornerRadius radii{
          cornerRadius.topLeft * radiusScale,
          cornerRadius.topRight * radiusScale,
          cornerRadius.bottomRight * radiusScale,
          cornerRadius.bottomLeft * radiusScale,
      };
      constexpr float eps = 1e-3f;
      bool const clipped =
          std::abs(effective.x - r.x) > eps || std::abs(effective.y - r.y) > eps ||
          std::abs(effective.width - r.width) > eps || std::abs(effective.height - r.height) > eps;
      if (clipped) {
        radii = cornerRadiiAfterAxisAlignedClip(r, effective, radii);
      }
      CornerRadius const adjusted = clampRadii(radii, effective.width, effective.height);
      if (!adjusted.isZero() && effective.width > 0.f && effective.height > 0.f &&
          state_.roundedClipCount < kVulkanRoundedClipMaskCapacity) {
        state_.roundedClips[state_.roundedClipCount++] = RoundedClipState{
            .rect = effective,
            .radii = adjusted,
        };
      }
    }
  }
  Rect clipBounds() const override { return state_.clip; }
  bool quickReject(Rect rect) const override { return !state_.clip.intersects(transformedBounds(rect)); }
  void setOpacity(float opacity) override { state_.opacity = std::clamp(opacity, 0.f, 1.f); }
  float opacity() const override { return state_.opacity; }
  void setBlendMode(BlendMode mode) override { state_.blendMode = mode; }
  BlendMode blendMode() const override { return state_.blendMode; }

  void pushRectInstance(Rect const &rect, CornerRadius const &cornerRadius, FillStyle const &fill,
                        StrokeStyle const &stroke, float opacity) {
    if (rect.width <= 0.f || rect.height <= 0.f)
      return;
    Point const p0 = state_.transform.apply({rect.x, rect.y});
    Point const p1 = state_.transform.apply({rect.x + rect.width, rect.y});
    Point const p3 = state_.transform.apply({rect.x, rect.y + rect.height});
    RectInstance inst{};
    inst.rect[0] = 0.f;
    inst.rect[1] = 0.f;
    inst.rect[2] = rect.width;
    inst.rect[3] = rect.height;
    inst.axisX[0] = p0.x;
    inst.axisX[1] = p0.y;
    inst.axisX[2] = p1.x - p0.x;
    inst.axisX[3] = p1.y - p0.y;
    inst.axisY[0] = p3.x - p0.x;
    inst.axisY[1] = p3.y - p0.y;
    CornerRadius cr = clampRadii(cornerRadius, rect.width, rect.height);
    inst.radii[0] = cr.topLeft;
    inst.radii[1] = cr.topRight;
    inst.radii[2] = cr.bottomRight;
    inst.radii[3] = cr.bottomLeft;
    encodeFill(fill, inst);
    Color sc{};
    if (stroke.solidColor(&sc) && stroke.width > 0.f) {
      putColor(inst.stroke, sc, 1.f);
      inst.params[2] = stroke.width;
    }
    inst.params[3] = opacity;
    applyCurrentRoundedClip(inst);
    RecordingTarget target = recordingTarget();
    std::uint32_t first = static_cast<std::uint32_t>(target.rects.size());
    target.rects.push_back(inst);
    appendSignedDrawOp(target, makeDrawOp(DrawOp::Kind::Rect, nullptr, first, 1));
    debug::perf::recordDrawCall(debug::perf::RenderCounterKind::Rect);
  }

  void drawRect(Rect const &rect, CornerRadius const &cornerRadius, FillStyle const &fill,
                StrokeStyle const &stroke, ShadowStyle const &shadow) override {
    if (rect.width <= 0.f || rect.height <= 0.f)
      return;
    Rect rejectBounds = transformedBounds(rect);
    if (!shadow.isNone()) {
      float const pad = shadow.radius;
      Rect const shadowRect = Rect::sharp(rect.x + shadow.offset.x - pad, rect.y + shadow.offset.y - pad,
                                          rect.width + pad * 2.f, rect.height + pad * 2.f);
      rejectBounds = unionRects(rejectBounds, transformedBounds(shadowRect));
    }
    if (rejectBounds.width <= 0.f || rejectBounds.height <= 0.f || !state_.clip.intersects(rejectBounds))
      return;

    if (!shadow.isNone()) {
      if (shadow.radius <= 0.f) {
        Rect const layer = Rect::sharp(rect.x + shadow.offset.x, rect.y + shadow.offset.y,
                                       rect.width, rect.height);
        pushRectInstance(layer, cornerRadius, FillStyle::solid(shadow.color), StrokeStyle::none(), state_.opacity);
      } else {
        int const steps = std::clamp(static_cast<int>(std::ceil(shadow.radius / 3.f)), 3, 8);
        for (int i = steps; i >= 1; --i) {
          float const t = static_cast<float>(i) / static_cast<float>(steps);
          float const spread = shadow.radius * t;
          float const alpha = shadow.color.a * state_.opacity * (1.f - t * 0.72f) / static_cast<float>(steps);
          Color c = shadow.color;
          c.a = alpha;
          Rect const layer = Rect::sharp(rect.x + shadow.offset.x - spread,
                                         rect.y + shadow.offset.y - spread,
                                         rect.width + spread * 2.f,
                                         rect.height + spread * 2.f);
          CornerRadius cr{cornerRadius.topLeft + spread, cornerRadius.topRight + spread,
                          cornerRadius.bottomRight + spread, cornerRadius.bottomLeft + spread};
          pushRectInstance(layer, cr, FillStyle::solid(c), StrokeStyle::none(), 1.f);
        }
      }
    }

    pushRectInstance(rect, cornerRadius, fill, stroke, state_.opacity);
  }

  void drawLine(Point from, Point to, StrokeStyle const &stroke) override {
    if (stroke.isNone())
      return;
    Point a = state_.transform.apply(from);
    Point b = state_.transform.apply(to);
    if (!clipLineToCurrentClip(a, b))
      return;
    StrokeStyle scaled = stroke;
    float const sx = std::hypot(state_.transform.m[0], state_.transform.m[1]);
    float const sy = std::hypot(state_.transform.m[3], state_.transform.m[4]);
    float const s = (sx > 0.f || sy > 0.f) ? (sx + sy) * 0.5f : 1.f;
    scaled.width *= s;
    Path path;
    path.moveTo(a);
    path.lineTo(b);
    DrawState saved = state_;
    state_.transform = Mat3::identity();
    appendPath(path, FillStyle::none(), scaled);
    state_ = saved;
  }
  void drawPath(Path const &path, FillStyle const &fill, StrokeStyle const &stroke, ShadowStyle const &shadow) override {
    if (!shadow.isNone()) {
      DrawState saved = state_;
      state_.transform = state_.transform * Mat3::translate(shadow.offset);
      appendPath(path, FillStyle::solid(shadow.color), StrokeStyle::none());
      state_ = saved;
    }
    appendPath(path, fill, stroke);
  }
  void drawCircle(Point center, float radius, FillStyle const &fill, StrokeStyle const &stroke) override {
    Rect r{center.x - radius, center.y - radius, radius * 2.f, radius * 2.f};
    drawRect(r, CornerRadius::pill(r), fill, stroke, ShadowStyle::none());
  }

  void drawTextLayout(TextLayout const &layout, Point origin) override {
    try {
      ensureAtlasDescriptor();
    } catch (std::exception const &e) {
      std::fprintf(stderr, "Lambda Vulkan: glyph atlas descriptor setup failed: %s\n", e.what());
      return;
    }
    RecordingTarget target = recordingTarget();
    std::uint32_t first = static_cast<std::uint32_t>(target.quads.size());
    for (TextLayout::PlacedRun const &placed : layout.runs) {
      std::size_t const glyphCount = std::min(placed.run.glyphIds.size(), placed.run.positions.size());
      for (std::size_t i = 0; i < glyphCount; ++i) {
        VulkanGlyphSlot const *slot = nullptr;
        try {
          slot = glyphSlot(placed.run.fontId, placed.run.glyphIds[i], placed.run.fontSize);
        } catch (std::exception const &e) {
          std::fprintf(stderr, "Lambda Vulkan: glyph atlas update failed: %s\n", e.what());
          continue;
        }
        if (!slot || slot->w == 0 || slot->h == 0)
          continue;
        Point pos = origin + placed.origin + placed.run.positions[i];
        Rect glyphRect = Rect::sharp(pos.x + slot->bearing.x / dpiScaleX_,
                                     pos.y - slot->bearing.y / dpiScaleY_,
                                     static_cast<float>(slot->w) / dpiScaleX_,
                                     static_cast<float>(slot->h) / dpiScaleY_);
        Point p00 = state_.transform.apply({glyphRect.x, glyphRect.y});
        Point p10 = state_.transform.apply({glyphRect.x + glyphRect.width, glyphRect.y});
        Point p01 = state_.transform.apply({glyphRect.x, glyphRect.y + glyphRect.height});
        Point const axisX = {p10.x - p00.x, p10.y - p00.y};
        Point const axisY = {p01.x - p00.x, p01.y - p00.y};
        p00.x = std::round(p00.x * dpiScaleX_) / dpiScaleX_;
        p00.y = std::round(p00.y * dpiScaleY_) / dpiScaleY_;
        p10 = {p00.x + axisX.x, p00.y + axisX.y};
        p01 = {p00.x + axisY.x, p00.y + axisY.y};
        QuadInstance q{};
        q.rect[0] = 0.f;
        q.rect[1] = 0.f;
        q.rect[2] = glyphRect.width;
        q.rect[3] = glyphRect.height;
        q.axisX[0] = p00.x;
        q.axisX[1] = p00.y;
        q.axisX[2] = p10.x - p00.x;
        q.axisX[3] = p10.y - p00.y;
        q.axisY[0] = p01.x - p00.x;
        q.axisY[1] = p01.y - p00.y;
        q.uv[0] = slot->u0;
        q.uv[1] = slot->v0;
        q.uv[2] = slot->u1;
        q.uv[3] = slot->v1;
        putColor(q.color, placed.run.color, state_.opacity);
        applyCurrentRoundedClip(q);
        target.quads.push_back(q);
      }
    }
    std::uint32_t count = static_cast<std::uint32_t>(target.quads.size()) - first;
    if (count > 0) {
      Texture *atlas = &resources().atlas;
      if (captureTarget_) {
        captureTarget_->glyphAtlasGeneration = resources().atlasGeneration;
      } else {
        batches_.push_back(ImageBatch{atlas, first, count});
      }
      appendSignedDrawOp(target, makeDrawOp(DrawOp::Kind::Image, atlas, first, count));
      debug::perf::recordDrawCall(debug::perf::RenderCounterKind::Glyph);
    }
  }

  void drawImage(Image const &image, Rect const &src, Rect const &dst, CornerRadius const &corners, float opacity) override {
    auto const *vi = dynamic_cast<VulkanImage const *>(&image);
    if (!vi || src.width <= 0.f || src.height <= 0.f || dst.width <= 0.f || dst.height <= 0.f)
      return;
    Texture *texture = nullptr;
    try {
      texture = ensureImageTexture(*vi);
    } catch (std::exception const &e) {
      std::fprintf(stderr, "Lambda Vulkan: image texture upload failed: %s\n", e.what());
      return;
    }
    if (!texture)
      return;
    Point p00 = state_.transform.apply({dst.x, dst.y});
    Point p10 = state_.transform.apply({dst.x + dst.width, dst.y});
    Point p01 = state_.transform.apply({dst.x, dst.y + dst.height});
    Size sz = image.size();
    float const u0 = src.x / sz.width;
    float const v0 = src.y / sz.height;
    float const u1 = (src.x + src.width) / sz.width;
    float const v1 = (src.y + src.height) / sz.height;
    QuadInstance q{};
    q.rect[0] = 0.f;
    q.rect[1] = 0.f;
    q.rect[2] = dst.width;
    q.rect[3] = dst.height;
    q.axisX[0] = p00.x;
    q.axisX[1] = p00.y;
    q.axisX[2] = p10.x - p00.x;
    q.axisX[3] = p10.y - p00.y;
    q.axisY[0] = p01.x - p00.x;
    q.axisY[1] = p01.y - p00.y;
    q.uv[0] = u0;
    q.uv[1] = v0;
    q.uv[2] = u1;
    q.uv[3] = v1;
    q.color[0] = q.color[1] = q.color[2] = 1.f;
    q.color[3] = opacity * state_.opacity;
    CornerRadius cr = clampRadii(corners, dst.width, dst.height);
    q.radii[0] = cr.topLeft;
    q.radii[1] = cr.topRight;
    q.radii[2] = cr.bottomRight;
    q.radii[3] = cr.bottomLeft;
    applyCurrentRoundedClip(q);
    RecordingTarget target = recordingTarget();
    std::uint32_t first = static_cast<std::uint32_t>(target.quads.size());
    target.quads.push_back(q);
    if (!captureTarget_) {
      batches_.push_back(ImageBatch{texture, first, 1});
    }
    DrawOp op = makeDrawOp(DrawOp::Kind::Image, texture, first, 1);
    op.sourceImage = vi;
    try {
      op.sourceImageRef = image.shared_from_this();
    } catch (std::bad_weak_ptr const&) {
      op.sourceImageRef.reset();
    }
    op.premultipliedAlpha = imagePremultipliedAlpha_ || image.premultipliedAlpha();
    appendSignedDrawOp(target, op);
    debug::perf::recordDrawCall(debug::perf::RenderCounterKind::Image);
  }

  void drawImageTiled(Image const &image, Rect const &dst, CornerRadius const &corners, float opacity) override {
    Size sz = image.size();
    if (sz.width <= 0.f || sz.height <= 0.f || dst.width <= 0.f || dst.height <= 0.f)
      return;
    int const cols = static_cast<int>(std::ceil(dst.width / sz.width));
    int const rows = static_cast<int>(std::ceil(dst.height / sz.height));
    for (int row = 0; row < rows; ++row) {
      for (int col = 0; col < cols; ++col) {
        Rect tile = Rect::sharp(dst.x + static_cast<float>(col) * sz.width,
                                dst.y + static_cast<float>(row) * sz.height,
                                std::min(sz.width, dst.x + dst.width - (dst.x + static_cast<float>(col) * sz.width)),
                                std::min(sz.height, dst.y + dst.height - (dst.y + static_cast<float>(row) * sz.height)));
        Rect src = Rect::sharp(0.f, 0.f, tile.width, tile.height);
        drawImage(image, src, tile, corners, opacity);
      }
    }
  }

  void drawBackdropBlur(Rect const &rect, float radius, Color tint, CornerRadius const &corners) override {
    drawBackdropBlurCached(rect, rect, radius, tint, corners);
  }

  void drawBackdropBlurCached(Rect const &rect,
                              Rect const &cacheRect,
                              float radius,
                              Color tint,
                              CornerRadius const &corners) override {
    if (radius <= 0.f || rect.width <= 0.f || rect.height <= 0.f)
      return;
    Rect const bounds = transformedBounds(rect);
    if (!state_.clip.intersects(bounds))
      return;

    RecordingTarget target = recordingTarget();
    std::uint32_t first = static_cast<std::uint32_t>(target.quads.size());
    appendBackdropBlurQuadInstance(target, rect, tint, corners);
    DrawOp op = makeDrawOp(DrawOp::Kind::BackdropBlur, nullptr, first, 1);
    op.blurRadius = radius * std::max(dpiScaleX_, dpiScaleY_);
    Rect const cacheBounds = transformedBounds(cacheRect);
    if (cacheBounds.width > 0.f && cacheBounds.height > 0.f) {
      op.blurCacheClip = cacheBounds;
      op.hasBlurCacheClip = true;
    }
    appendSignedDrawOp(target, op);
  }

  bool drawBackdropBlurFrame(Rect const& frame,
                             CornerRadius const& frameRadius,
                             Rect const& cutout,
                             float radius,
                             Color tint) {
    if (radius <= 0.f || frame.width <= 0.f || frame.height <= 0.f) {
      return false;
    }
    if (!state_.clip.intersects(transformedBounds(frame))) {
      return true;
    }

    float const left = std::clamp(cutout.x - frame.x, 0.f, frame.width);
    float const top = std::clamp(cutout.y - frame.y, 0.f, frame.height);
    float const right = std::clamp(frame.x + frame.width - (cutout.x + cutout.width), 0.f, frame.width);
    float const bottom = std::clamp(frame.y + frame.height - (cutout.y + cutout.height), 0.f, frame.height);

    RecordingTarget target = recordingTarget();
    std::uint32_t const first = static_cast<std::uint32_t>(target.quads.size());
    if (top > 0.f) {
      appendBackdropBlurQuadInstance(target,
                                     Rect::sharp(frame.x, frame.y, frame.width, top),
                                     tint,
                                     CornerRadius{frameRadius.topLeft, frameRadius.topRight, 0.f, 0.f});
    }
    if (left > 0.f && cutout.height > 0.f) {
      appendBackdropBlurQuadInstance(target,
                                     Rect::sharp(frame.x, cutout.y, left, cutout.height),
                                     tint,
                                     CornerRadius{});
    }
    if (right > 0.f && cutout.height > 0.f) {
      appendBackdropBlurQuadInstance(target,
                                     Rect::sharp(cutout.x + cutout.width, cutout.y, right, cutout.height),
                                     tint,
                                     CornerRadius{});
    }
    if (bottom > 0.f) {
      appendBackdropBlurQuadInstance(target,
                                     Rect::sharp(frame.x, cutout.y + cutout.height, frame.width, bottom),
                                     tint,
                                     CornerRadius{0.f, 0.f, frameRadius.bottomRight, frameRadius.bottomLeft});
    }

    std::uint32_t const count = static_cast<std::uint32_t>(target.quads.size()) - first;
    if (count == 0) {
      return true;
    }
    DrawOp op = makeDrawOp(DrawOp::Kind::BackdropBlur, nullptr, first, count);
    op.blurRadius = radius * std::max(dpiScaleX_, dpiScaleY_);
    Rect const cacheBounds = transformedBounds(frame);
    if (cacheBounds.width > 0.f && cacheBounds.height > 0.f) {
      op.blurCacheClip = cacheBounds;
      op.hasBlurCacheClip = true;
    }
    appendSignedDrawOp(target, op);
    return true;
  }

  bool drawCalloutMaterial(Rect const &bounds,
                           Rect const &card,
                           CornerRadius const &corners,
                           Color baseColor,
                           Color tintColor,
                           Color borderColor,
                           float borderWidth,
                           VulkanCalloutPlacement placement,
                           float arrowWidth,
                           float) {
    if (captureTarget_ || bounds.width <= 0.f || bounds.height <= 0.f || card.width <= 0.f || card.height <= 0.f)
      return false;
    Rect const transformed = transformedBounds(bounds);
    if (!state_.clip.intersects(transformed))
      return true;

    Point p00 = state_.transform.apply({bounds.x, bounds.y});
    Point p10 = state_.transform.apply({bounds.x + bounds.width, bounds.y});
    Point p01 = state_.transform.apply({bounds.x, bounds.y + bounds.height});
    CalloutInstance inst{};
    inst.rect[0] = 0.f;
    inst.rect[1] = 0.f;
    inst.rect[2] = bounds.width;
    inst.rect[3] = bounds.height;
    inst.axisX[0] = p00.x;
    inst.axisX[1] = p00.y;
    inst.axisX[2] = p10.x - p00.x;
    inst.axisX[3] = p10.y - p00.y;
    inst.axisY[0] = p01.x - p00.x;
    inst.axisY[1] = p01.y - p00.y;
    inst.card[0] = card.x - bounds.x;
    inst.card[1] = card.y - bounds.y;
    inst.card[2] = card.width;
    inst.card[3] = card.height;
    CornerRadius cr = clampRadii(corners, card.width, card.height);
    inst.radii[0] = cr.topLeft;
    inst.radii[1] = cr.topRight;
    inst.radii[2] = cr.bottomRight;
    inst.radii[3] = cr.bottomLeft;
    putColor(inst.base, baseColor, state_.opacity);
    putColor(inst.tint, tintColor, state_.opacity);
    putColor(inst.stroke, borderColor, state_.opacity);
    inst.params[0] = std::max(0.f, borderWidth);
    inst.params[1] = static_cast<float>(placement);
    inst.params[2] = std::max(0.f, arrowWidth);
    applyCurrentRoundedClip(inst);

    std::uint32_t const first = static_cast<std::uint32_t>(callouts_.size());
    callouts_.push_back(inst);
    DrawOp op = makeDrawOp(DrawOp::Kind::Callout, nullptr, first, 1);
    std::uint64_t signature = 14695981039346656037ULL;
    hashValue(signature, inst);
    op.geometrySignature = nonZeroSignature(signature);
    appendDrawOp(ops_, op);
    return true;
  }

  void *gpuDevice() const override { return device_; }

  void evictImageTexture(VulkanImage const *image) {
    auto it = imageTextures_.find(image);
    if (it == imageTextures_.end())
      return;
    pendingTextureDestroys_.push_back(PendingTextureDestroy{std::move(it->second), kMaxFramesInFlight + 1u});
    imageTextures_.erase(it);
  }

  bool deferOwnedImageDestroy(PendingOwnedVulkanImageDestroy destroy) {
    if (!destroy.device || destroy.device != device_) {
      return false;
    }
    if (destroy.allocator && destroy.allocator != allocator_) {
      return false;
    }
    destroy.framesRemaining = kMaxFramesInFlight + 1u;
    pendingOwnedImageDestroys_.push_back(destroy);
    return true;
  }

  void updateImageTexture(VulkanImage const* image) {
    if (!image || image->external || image->ownsGpuResource) {
      return;
    }
    auto it = imageTextures_.find(image);
    if (it == imageTextures_.end() || !it->second) {
      return;
    }
    Texture& texture = *it->second;
    if (texture.width != image->width || texture.height != image->height) {
      evictImageTexture(image);
      return;
    }
    renderTargetFrameCacheValid_ = false;
    texture.contentGeneration = image->contentGeneration;
    queueTextureUpload(texture, image->pixels.data());
  }

  void updateImageTextureRegion(VulkanImage const* image,
                                std::uint32_t x,
                                std::uint32_t y,
                                std::uint32_t width,
                                std::uint32_t height,
                                void const* pixels,
                                std::uint32_t sourceBytesPerRow = 0) {
    if (!image || image->external || image->ownsGpuResource || !pixels || width == 0 || height == 0) {
      return;
    }
    auto it = imageTextures_.find(image);
    if (it == imageTextures_.end() || !it->second) {
      return;
    }
    Texture& texture = *it->second;
    if (texture.width != image->width || texture.height != image->height) {
      evictImageTexture(image);
      return;
    }
    if (x > static_cast<std::uint32_t>(texture.width) || y > static_cast<std::uint32_t>(texture.height) ||
        width > static_cast<std::uint32_t>(texture.width) - x ||
        height > static_cast<std::uint32_t>(texture.height) - y) {
      return;
    }
    renderTargetFrameCacheValid_ = false;
    texture.contentGeneration = image->contentGeneration;
    queueTextureRegionUpload(texture, x, y, width, height, pixels, sourceBytesPerRow);
  }

  void markImageTextureContentsChanged(VulkanImage const* image) {
    if (!image) {
      return;
    }
    auto it = imageTextures_.find(image);
    if (it == imageTextures_.end() || !it->second) {
      return;
    }
    it->second->contentGeneration = image->contentGeneration;
    renderTargetFrameCacheValid_ = false;
  }

private:
  struct PendingTextureDestroy {
    std::unique_ptr<Texture> texture;
    std::uint32_t framesRemaining = 0;
    std::uint32_t successfulSubmits = 0;
  };

  struct PendingTextureUpload {
    Texture* texture = nullptr;
    Buffer staging;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
  };

  struct PendingTextureTransition {
    Texture* texture = nullptr;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
  };

  struct PendingBufferDestroy {
    Buffer buffer;
    std::uint32_t framesRemaining = 0;
    std::uint32_t successfulSubmits = 0;
    bool recycleUploadStaging = false;
  };

  struct PendingRecorderResourceDestroy {
    VulkanFrameRecorderResources resources;
    std::uint32_t framesRemaining = 0;
    std::uint32_t successfulSubmits = 0;
  };

  struct CachedBackdropBlur {
    std::uint64_t signature = 0;
    Texture texture;
    bool valid = false;
  };

  struct RoundedClipState {
    Rect rect{};
    CornerRadius radii{};
  };

  struct DrawState {
    Mat3 transform = Mat3::identity();
    Rect clip{};
    std::array<RoundedClipState, kVulkanRoundedClipMaskCapacity> roundedClips{};
    std::uint32_t roundedClipCount = 0;
    float opacity = 1.f;
    BlendMode blendMode = BlendMode::Normal;
  };

  struct RecordingTarget {
    std::vector<DrawOp> &ops;
    std::vector<QuadInstance> &quads;
    std::vector<RectInstance> &rects;
    std::vector<VulkanPathVertex> &pathVerts;
  };

  RecordingTarget recordingTarget() {
    if (captureTarget_) {
      return RecordingTarget{captureTarget_->ops, captureTarget_->quads, captureTarget_->rects,
                             captureTarget_->pathVerts};
    }
    return RecordingTarget{ops_, quads_, rects_, pathVerts_};
  }

  void appendBackdropBlurQuadInstance(RecordingTarget& target,
                                      Rect const& rect,
                                      Color tint,
                                      CornerRadius const& corners) const {
    Point p00 = state_.transform.apply({rect.x, rect.y});
    Point p10 = state_.transform.apply({rect.x + rect.width, rect.y});
    Point p01 = state_.transform.apply({rect.x, rect.y + rect.height});
    QuadInstance q{};
    q.rect[0] = 0.f;
    q.rect[1] = 0.f;
    q.rect[2] = rect.width;
    q.rect[3] = rect.height;
    q.axisX[0] = p00.x;
    q.axisX[1] = p00.y;
    q.axisX[2] = p10.x - p00.x;
    q.axisX[3] = p10.y - p00.y;
    q.axisY[0] = p01.x - p00.x;
    q.axisY[1] = p01.y - p00.y;
    q.uv[0] = p00.x / std::max(1.f, static_cast<float>(width_));
    q.uv[1] = p00.y / std::max(1.f, static_cast<float>(height_));
    q.uv[2] = p10.x / std::max(1.f, static_cast<float>(width_));
    q.uv[3] = p01.y / std::max(1.f, static_cast<float>(height_));
    putColor(q.color, tint, state_.opacity);
    CornerRadius cr = clampRadii(corners, rect.width, rect.height);
    q.radii[0] = cr.topLeft;
    q.radii[1] = cr.topRight;
    q.radii[2] = cr.bottomRight;
    q.radii[3] = cr.bottomLeft;
    applyCurrentRoundedClip(q);
    target.quads.push_back(q);
  }

  void registerCanvas() {
    std::lock_guard lock(gCanvasRegistryMutex);
    gCanvases.push_back(this);
  }

  void unregisterCanvas() {
    std::lock_guard lock(gCanvasRegistryMutex);
    gCanvases.erase(std::remove(gCanvases.begin(), gCanvases.end(), this), gCanvases.end());
  }

  Rect transformedBounds(Rect rect) const {
    return boundsOfTransformedRect(rect, state_.transform);
  }

  DrawOp makeDrawOp(DrawOp::Kind kind, Texture *texture, std::uint32_t first, std::uint32_t count) const {
    DrawOp op{};
    op.kind = kind;
    op.texture = texture;
    op.first = first;
    op.count = count;
    op.clip = state_.clip;
    return op;
  }

  void appendSignedDrawOp(RecordingTarget& target, DrawOp op) const {
    appendDrawOp(target.ops, op);
  }

  static bool sameRect(Rect a, Rect b) {
    constexpr float eps = 1e-4f;
    return std::abs(a.x - b.x) <= eps &&
           std::abs(a.y - b.y) <= eps &&
           std::abs(a.width - b.width) <= eps &&
           std::abs(a.height - b.height) <= eps;
  }

  static bool canMergeDrawOps(DrawOp const &a, DrawOp const &b) {
    return a.kind == b.kind &&
           a.texture == b.texture &&
           a.first + a.count == b.first &&
           sameRect(a.clip, b.clip) &&
           a.hasBlurCacheClip == b.hasBlurCacheClip &&
           (!a.hasBlurCacheClip || sameRect(a.blurCacheClip, b.blurCacheClip)) &&
           a.sourceImage == b.sourceImage &&
           a.externalStorageDescriptor == b.externalStorageDescriptor &&
           a.externalVertexBuffer == b.externalVertexBuffer &&
           a.premultipliedAlpha == b.premultipliedAlpha &&
           std::abs(a.externalTranslationX - b.externalTranslationX) <= 1e-4f &&
           std::abs(a.externalTranslationY - b.externalTranslationY) <= 1e-4f;
  }

  static void appendDrawOp(std::vector<DrawOp> &ops, DrawOp op) {
    if (!ops.empty() && canMergeDrawOps(ops.back(), op)) {
      DrawOp &prev = ops.back();
      std::uint64_t const previousSignature = prev.geometrySignature;
      std::uint64_t const nextSignature = op.geometrySignature;
      prev.count += op.count;
      prev.blurRadius = std::max(prev.blurRadius, op.blurRadius);
      if (previousSignature != 0 && nextSignature != 0) {
        std::uint64_t h = 14695981039346656037ULL;
        hashValue(h, previousSignature);
        hashValue(h, nextSignature);
        hashValue(h, prev.count);
        hashValue(h, prev.blurRadius);
        prev.geometrySignature = nonZeroSignature(h);
      } else {
        prev.geometrySignature = 0;
      }
      return;
    }
    ops.push_back(op);
  }

  static Rect translatedRect(Rect rect, float dx, float dy) {
    rect.x += dx;
    rect.y += dy;
    return rect;
  }

  Rect localReplayClip(VulkanFrameRecorder const& recorded, Rect opClip, float dx, float dy) const {
    if (sameRect(opClip, recorded.rootClip)) {
      return state_.clip;
    }
    return intersectRects(translatedRect(opClip, dx, dy), state_.clip);
  }

  static std::uint32_t roundedClipCount(float const (&header)[4]) {
    return std::min<std::uint32_t>(
        kVulkanRoundedClipMaskCapacity,
        static_cast<std::uint32_t>(std::max(0.f, header[0]) + 0.5f));
  }

  template <typename Instance>
  static void writeRoundedClip(Instance &inst, std::uint32_t index, RoundedClipState const &clip) {
    inst.clipEntries[index * 2u][0] = clip.rect.x;
    inst.clipEntries[index * 2u][1] = clip.rect.y;
    inst.clipEntries[index * 2u][2] = clip.rect.width;
    inst.clipEntries[index * 2u][3] = clip.rect.height;
    inst.clipEntries[index * 2u + 1u][0] = clip.radii.topLeft;
    inst.clipEntries[index * 2u + 1u][1] = clip.radii.topRight;
    inst.clipEntries[index * 2u + 1u][2] = clip.radii.bottomRight;
    inst.clipEntries[index * 2u + 1u][3] = clip.radii.bottomLeft;
  }

  template <typename Instance>
  static void translateRoundedClipStack(Instance &inst, float dx, float dy) {
    std::uint32_t const count = roundedClipCount(inst.clipHeader);
    for (std::uint32_t i = 0; i < count; ++i) {
      inst.clipEntries[i * 2u][0] += dx;
      inst.clipEntries[i * 2u][1] += dy;
    }
  }

  template <typename Instance>
  void applyCurrentRoundedClip(Instance &inst) const {
    if (state_.roundedClipCount == 0) {
      return;
    }
    std::array<float, 4> oldHeader{};
    std::copy(std::begin(inst.clipHeader), std::end(inst.clipHeader), oldHeader.begin());
    std::array<std::array<float, 4>, kVulkanRoundedClipEntryCount> oldEntries{};
    for (std::size_t i = 0; i < kVulkanRoundedClipEntryCount; ++i) {
      std::copy(std::begin(inst.clipEntries[i]), std::end(inst.clipEntries[i]), oldEntries[i].begin());
    }
    std::uint32_t const oldCount = std::min<std::uint32_t>(
        kVulkanRoundedClipMaskCapacity,
        static_cast<std::uint32_t>(std::max(0.f, oldHeader[0]) + 0.5f));

    inst.clipHeader[0] = 0.f;
    for (auto &entry : inst.clipEntries) {
      entry[0] = 0.f;
      entry[1] = 0.f;
      entry[2] = 0.f;
      entry[3] = 0.f;
    }

    std::uint32_t writeCount = 0;
    for (std::uint32_t i = 0; i < state_.roundedClipCount && writeCount < kVulkanRoundedClipMaskCapacity; ++i) {
      writeRoundedClip(inst, writeCount++, state_.roundedClips[i]);
    }
    for (std::uint32_t i = 0; i < oldCount && writeCount < kVulkanRoundedClipMaskCapacity; ++i) {
      inst.clipEntries[writeCount * 2u][0] = oldEntries[i * 2u][0];
      inst.clipEntries[writeCount * 2u][1] = oldEntries[i * 2u][1];
      inst.clipEntries[writeCount * 2u][2] = oldEntries[i * 2u][2];
      inst.clipEntries[writeCount * 2u][3] = oldEntries[i * 2u][3];
      inst.clipEntries[writeCount * 2u + 1u][0] = oldEntries[i * 2u + 1u][0];
      inst.clipEntries[writeCount * 2u + 1u][1] = oldEntries[i * 2u + 1u][1];
      inst.clipEntries[writeCount * 2u + 1u][2] = oldEntries[i * 2u + 1u][2];
      inst.clipEntries[writeCount * 2u + 1u][3] = oldEntries[i * 2u + 1u][3];
      ++writeCount;
    }
    inst.clipHeader[0] = static_cast<float>(writeCount);
  }

  std::uint64_t currentRoundedClipHash() const {
    std::uint64_t h = 14695981039346656037ULL;
    hashValue(h, state_.roundedClipCount);
    for (std::uint32_t i = 0; i < state_.roundedClipCount; ++i) {
      hashValue(h, state_.roundedClips[i].rect);
      hashValue(h, state_.roundedClips[i].radii);
    }
    return h;
  }

  static std::uint64_t nonZeroSignature(std::uint64_t value) {
    return value != 0 ? value : 1;
  }

  static std::uint64_t translatedGeometrySignature(std::uint64_t baseSignature,
                                                   float dx,
                                                   float dy,
                                                   float opacityScale) {
    if (baseSignature == 0) {
      return 0;
    }
    std::uint64_t h = 14695981039346656037ULL;
    hashValue(h, baseSignature);
    hashValue(h, dx);
    hashValue(h, dy);
    hashValue(h, opacityScale);
    return nonZeroSignature(h);
  }

  bool recordedGlyphAtlasCurrent(VulkanFrameRecorder const &recorded) const {
    if (!recorded.replayable) {
      return false;
    }
    return recorded.glyphAtlasGeneration == 0 ||
           recorded.glyphAtlasGeneration == resources().atlasGeneration;
  }

  void uploadRecorderBuffer(VmaAllocation allocation, void const *data, VkDeviceSize size) {
    if (!data || size == 0) {
      return;
    }
    void *mapped = nullptr;
    vkCheck(vmaMapMemory(allocator_, allocation, &mapped), "vmaMapMemory");
    std::memcpy(mapped, data, static_cast<std::size_t>(size));
    vkCheck(vmaFlushAllocation(allocator_, allocation, 0, size), "vmaFlushAllocation");
    vmaUnmapMemory(allocator_, allocation);
  }

  static std::uint64_t recorderBufferUploadSignature(void const *data,
                                                     VkDeviceSize size,
                                                     VkBufferUsageFlags usage) {
    if (!data || size == 0) {
      return 0;
    }
    std::uint64_t h = 14695981039346656037ULL;
    hashValue(h, size);
    hashValue(h, usage);
    hashBytes(h, data, static_cast<std::size_t>(size));
    return nonZeroSignature(h);
  }

  bool prepareRecorderOwnership(VulkanFrameRecorder const &recorded) {
    if ((recorded.allocator && recorded.allocator != allocator_) ||
        (recorded.device && recorded.device != device_) ||
        (recorded.descriptorPool && recorded.descriptorPool != resources().descriptorPool)) {
      return false;
    }
    if (!recorded.allocator) {
      recorded.allocator = allocator_;
    }
    if (!recorded.device) {
      recorded.device = device_;
    }
    if (!recorded.descriptorPool) {
      recorded.descriptorPool = resources().descriptorPool;
    }
    return true;
  }

  bool deferFrameRecorderResourcesDestroy(VulkanFrameRecorderResources retired) {
    if (!retired.device || retired.device != device_) {
      return false;
    }
    if (retired.allocator && retired.allocator != allocator_) {
      return false;
    }
    if (retired.descriptorPool && retired.descriptorPool != resources().descriptorPool) {
      return false;
    }
    pendingRecorderResourceDestroys_.push_back(PendingRecorderResourceDestroy{
        .resources = retired,
        .framesRemaining = kMaxFramesInFlight + 1u,
        .successfulSubmits = 0u,
    });
    return true;
  }

  bool ensureRecorderBuffer(VulkanFrameRecorder const &recorded, VkBuffer &buffer,
                            VmaAllocation &allocation, VkDeviceSize &capacity,
                            std::uint64_t &uploadSignature, VkDescriptorSet* descriptor,
                            void const *data,
                            VkDeviceSize size, VkBufferUsageFlags usage) {
    if (!data || size == 0) {
      return true;
    }
    if (!prepareRecorderOwnership(recorded))
      return false;
    std::uint64_t const nextSignature = recorderBufferUploadSignature(data, size, usage);
    if (buffer && capacity >= size) {
      if (uploadSignature == nextSignature) {
        return true;
      }
      uploadRecorderBuffer(allocation, data, size);
      uploadSignature = nextSignature;
      return true;
    }
    if (buffer) {
      VkDescriptorSet retiredDescriptor = VK_NULL_HANDLE;
      if (descriptor) {
        retiredDescriptor = *descriptor;
        *descriptor = VK_NULL_HANDLE;
      }
      retireVulkanFrameRecorderResources(VulkanFrameRecorderResources{
          .allocator = recorded.allocator,
          .device = recorded.device,
          .descriptorPool = recorded.descriptorPool,
          .preparedQuadBuffer = buffer,
          .preparedQuadAllocation = allocation,
          .preparedQuadDescriptor = retiredDescriptor,
      });
      buffer = VK_NULL_HANDLE;
      allocation = VK_NULL_HANDLE;
      capacity = 0;
      uploadSignature = 0;
    }
    capacity = std::max<VkDeviceSize>(size, 1024);
    auto info = vkStructure<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    info.size = capacity;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    vkCheck(vmaCreateBuffer(allocator_, &info, &allocationInfo, &buffer, &allocation, nullptr),
            "vmaCreateBuffer");
    uploadRecorderBuffer(allocation, data, size);
    uploadSignature = nextSignature;
    return true;
  }

  bool recorderPreparedGeometryFastPathEnabled() const {
    int const overrideValue = vulkanPreparedGeometryOverride();
    if (overrideValue < 0) {
      return false;
    }
    if (overrideValue > 0) {
      return true;
    }
    return shared_ && shared_->driverId != VK_DRIVER_ID_MESA_RADV;
  }

  bool ensureRecorderStorageDescriptor(VulkanFrameRecorder const &recorded, VkDescriptorSet &set,
                                       VkDescriptorSetLayout layout, VkBuffer buffer,
                                       VkDeviceSize capacity) {
    if (!buffer || capacity == 0) {
      return true;
    }
    if (!prepareRecorderOwnership(recorded))
      return false;
    if (!set) {
      auto alloc = vkStructure<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
      alloc.descriptorPool = recorded.descriptorPool;
      alloc.descriptorSetCount = 1;
      alloc.pSetLayouts = &layout;
      vkCheck(vkAllocateDescriptorSets(device_, &alloc, &set), "vkAllocateDescriptorSets");
    }
    VkDescriptorBufferInfo bi{buffer, 0, capacity};
    auto write = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bi;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return true;
  }

  bool prepareRecorderBuffers(VulkanFrameRecorder const &recorded) {
    if (!recorderPreparedGeometryFastPathEnabled()) {
      return true;
    }
    if (!prepareRecorderOwnership(recorded))
      return false;
    if (!ensureRecorderBuffer(recorded, recorded.preparedRectBuffer, recorded.preparedRectAllocation,
                              recorded.preparedRectCapacity,
                              recorded.preparedRectUploadSignature,
                              &recorded.preparedRectDescriptor, recorded.rects.data(),
                              static_cast<VkDeviceSize>(recorded.rects.size() * sizeof(RectInstance)),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
      return false;
    }
    if (!ensureRecorderBuffer(recorded, recorded.preparedQuadBuffer, recorded.preparedQuadAllocation,
                              recorded.preparedQuadCapacity,
                              recorded.preparedQuadUploadSignature,
                              &recorded.preparedQuadDescriptor, recorded.quads.data(),
                              static_cast<VkDeviceSize>(recorded.quads.size() * sizeof(QuadInstance)),
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
      return false;
    }
    if (!ensureRecorderBuffer(recorded, recorded.preparedPathVertexBuffer,
                              recorded.preparedPathVertexAllocation,
                              recorded.preparedPathVertexCapacity,
                              recorded.preparedPathVertexUploadSignature, nullptr, recorded.pathVerts.data(),
                              static_cast<VkDeviceSize>(recorded.pathVerts.size() * sizeof(VulkanPathVertex)),
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
      return false;
    }
    if (!ensureRecorderStorageDescriptor(recorded, recorded.preparedRectDescriptor,
                                         resources().rectDescriptorLayout,
                                         recorded.preparedRectBuffer,
                                         recorded.preparedRectCapacity)) {
      return false;
    }
    if (!ensureRecorderStorageDescriptor(recorded, recorded.preparedQuadDescriptor,
                                         resources().quadDescriptorLayout,
                                         recorded.preparedQuadBuffer,
                                         recorded.preparedQuadCapacity)) {
      return false;
    }
    return true;
  }

  static void translateRectInstance(RectInstance &inst, float dx, float dy, float opacityScale) {
    inst.axisX[0] += dx;
    inst.axisX[1] += dy;
    inst.stroke[3] *= opacityScale;
    inst.params[3] *= opacityScale;
    translateRoundedClipStack(inst, dx, dy);
  }

  bool resolveRecordedImageTexture(DrawOp &op) {
    if (op.kind != DrawOp::Kind::Image || !op.sourceImage) {
      return true;
    }
    try {
      op.texture = ensureImageTexture(*static_cast<VulkanImage const *>(op.sourceImage));
    } catch (std::exception const &e) {
      std::fprintf(stderr, "Lambda Vulkan: recorded image texture upload failed: %s\n", e.what());
      return false;
    }
    return op.texture != nullptr;
  }

  static void translateQuadInstance(QuadInstance &inst, float dx, float dy, float opacityScale) {
    inst.axisX[0] += dx;
    inst.axisX[1] += dy;
    inst.color[3] *= opacityScale;
    translateRoundedClipStack(inst, dx, dy);
  }

  static void translatePathVertex(VulkanPathVertex &vertex, float dx, float dy, float opacityScale) {
    vertex.x += dx;
    vertex.y += dy;
    vertex.color[3] *= opacityScale;
    vertex.params[3] *= opacityScale;
    translateRoundedClipStack(vertex, dx, dy);
  }

  bool appendRecordedOps(VulkanFrameRecorder const &recorded, bool localReplay) {
    float const dx = localReplay ? state_.transform.m[6] : 0.f;
    float const dy = localReplay ? state_.transform.m[7] : 0.f;
    float const opacityScale = localReplay ? state_.opacity : 1.f;
    constexpr float eps = 1e-4f;
    bool const hasTranslatedBackdropBlur =
        localReplay && (std::abs(dx) > eps || std::abs(dy) > eps) &&
        std::any_of(recorded.ops.begin(), recorded.ops.end(), [](DrawOp const &op) {
          return op.kind == DrawOp::Kind::BackdropBlur;
        });
    bool const canUsePreparedGeometry =
        // RADV can crash while recording draws that bind prepared replay descriptors for cached composite
        // content. Keep that driver on the CPU-copy path while allowing unaffected drivers to bind replay buffers.
        recorderPreparedGeometryFastPathEnabled() &&
        (!localReplay || std::abs(opacityScale - 1.f) <= eps) &&
        (!localReplay || state_.roundedClipCount == 0) &&
        !hasTranslatedBackdropBlur &&
        (recorded.rects.empty() ||
         (recorded.preparedRectBuffer && recorded.preparedRectDescriptor)) &&
        (recorded.quads.empty() ||
         (recorded.preparedQuadBuffer && recorded.preparedQuadDescriptor)) &&
        (recorded.pathVerts.empty() || recorded.preparedPathVertexBuffer);

    std::vector<DrawOp> resolvedOps = recorded.ops;
    for (DrawOp &op : resolvedOps) {
      if (!resolveRecordedImageTexture(op)) {
        return false;
      }
    }

    if (canUsePreparedGeometry) {
      ops_.reserve(ops_.size() + resolvedOps.size());
      for (DrawOp op : resolvedOps) {
        switch (op.kind) {
        case DrawOp::Kind::Rect:
          op.externalStorageDescriptor = recorded.preparedRectDescriptor;
          break;
        case DrawOp::Kind::Callout:
          break;
        case DrawOp::Kind::Path:
          op.externalVertexBuffer = recorded.preparedPathVertexBuffer;
          break;
        case DrawOp::Kind::Image:
        case DrawOp::Kind::BackdropBlur:
          op.externalStorageDescriptor = recorded.preparedQuadDescriptor;
          break;
        }
        if (localReplay) {
          op.geometrySignature =
              translatedGeometrySignature(op.geometrySignature, dx, dy, opacityScale);
          op.clip = localReplayClip(recorded, op.clip, dx, dy);
          if (op.hasBlurCacheClip) {
            op.blurCacheClip = translatedRect(op.blurCacheClip, dx, dy);
          }
          op.externalTranslationX = dx;
          op.externalTranslationY = dy;
        }
        ops_.push_back(op);
      }
      return true;
    }

    std::uint32_t const rectBase = static_cast<std::uint32_t>(rects_.size());
    std::uint32_t const quadBase = static_cast<std::uint32_t>(quads_.size());
    std::uint32_t const pathBase = static_cast<std::uint32_t>(pathVerts_.size());

    rects_.reserve(rects_.size() + recorded.rects.size());
    for (RectInstance inst : recorded.rects) {
      if (localReplay) {
        translateRectInstance(inst, dx, dy, opacityScale);
        applyCurrentRoundedClip(inst);
      }
      rects_.push_back(inst);
    }

    quads_.reserve(quads_.size() + recorded.quads.size());
    for (QuadInstance inst : recorded.quads) {
      if (localReplay) {
        translateQuadInstance(inst, dx, dy, opacityScale);
        applyCurrentRoundedClip(inst);
      }
      quads_.push_back(inst);
    }

    pathVerts_.reserve(pathVerts_.size() + recorded.pathVerts.size());
    for (VulkanPathVertex vertex : recorded.pathVerts) {
      if (localReplay) {
        translatePathVertex(vertex, dx, dy, opacityScale);
        applyCurrentRoundedClip(vertex);
      }
      pathVerts_.push_back(vertex);
    }

    ops_.reserve(ops_.size() + resolvedOps.size());
    float const uvDx = localReplay ? dx / std::max(1.f, static_cast<float>(width_)) : 0.f;
    float const uvDy = localReplay ? dy / std::max(1.f, static_cast<float>(height_)) : 0.f;
    for (DrawOp op : resolvedOps) {
      std::uint32_t const originalFirst = op.first;
      switch (op.kind) {
      case DrawOp::Kind::Rect:
        op.first += rectBase;
        break;
      case DrawOp::Kind::Callout:
        break;
      case DrawOp::Kind::Path:
        op.first += pathBase;
        break;
      case DrawOp::Kind::Image:
        op.first += quadBase;
        break;
      case DrawOp::Kind::BackdropBlur:
        op.first += quadBase;
        if (localReplay) {
          for (std::uint32_t i = 0; i < op.count; ++i) {
            std::size_t const index = static_cast<std::size_t>(quadBase + originalFirst + i);
            if (index >= quads_.size()) {
              break;
            }
            QuadInstance &quad = quads_[index];
            quad.uv[0] += uvDx;
            quad.uv[1] += uvDy;
            quad.uv[2] += uvDx;
            quad.uv[3] += uvDy;
          }
        }
        break;
      }
      if (localReplay) {
        op.geometrySignature =
            translatedGeometrySignature(op.geometrySignature, dx, dy, opacityScale);
        op.clip = localReplayClip(recorded, op.clip, dx, dy);
        if (op.hasBlurCacheClip) {
          op.blurCacheClip = translatedRect(op.blurCacheClip, dx, dy);
        }
      }
      ops_.push_back(op);
    }
    return true;
  }

  bool clipLineToCurrentClip(Point &a, Point &b) const {
    float t0 = 0.f;
    float t1 = 1.f;
    float const dx = b.x - a.x;
    float const dy = b.y - a.y;
    float const xMin = state_.clip.x;
    float const yMin = state_.clip.y;
    float const xMax = state_.clip.x + state_.clip.width;
    float const yMax = state_.clip.y + state_.clip.height;
    auto edge = [&](float p, float q) {
      if (std::abs(p) < 1e-6f)
        return q >= 0.f;
      float const r = q / p;
      if (p < 0.f) {
        if (r > t1)
          return false;
        if (r > t0)
          t0 = r;
      } else {
        if (r < t0)
          return false;
        if (r < t1)
          t1 = r;
      }
      return true;
    };
    if (!edge(-dx, a.x - xMin) || !edge(dx, xMax - a.x) ||
        !edge(-dy, a.y - yMin) || !edge(dy, yMax - a.y)) {
      return false;
    }
    Point const original = a;
    a = {original.x + dx * t0, original.y + dy * t0};
    b = {original.x + dx * t1, original.y + dy * t1};
    return state_.clip.width > 0.f && state_.clip.height > 0.f;
  }

  void encodeFill(FillStyle const &fill, RectInstance &inst) {
    Color c{};
    if (fill.solidColor(&c)) {
      putColor(inst.fill0, c, 1.f);
      inst.stops[0] = 0.f;
      inst.params[1] = 1.f;
      return;
    }
    auto writeStops = [&](auto const &g) {
      inst.params[1] = static_cast<float>(g.stopCount);
      for (std::uint8_t i = 0; i < g.stopCount && i < 4; ++i) {
        float *colors[] = {inst.fill0, inst.fill1, inst.fill2, inst.fill3};
        putColor(colors[i], g.stops[i].color, 1.f);
        inst.stops[i] = g.stops[i].position;
      }
    };
    LinearGradient lg{};
    if (fill.linearGradient(&lg) && lg.stopCount > 0) {
      inst.params[0] = 1.f;
      inst.gradient[0] = lg.start.x;
      inst.gradient[1] = lg.start.y;
      inst.gradient[2] = lg.end.x;
      inst.gradient[3] = lg.end.y;
      writeStops(lg);
      return;
    }
    RadialGradient rg{};
    if (fill.radialGradient(&rg) && rg.stopCount > 0) {
      inst.params[0] = 2.f;
      inst.gradient[0] = rg.center.x;
      inst.gradient[1] = rg.center.y;
      inst.gradient[2] = rg.radius;
      writeStops(rg);
      return;
    }
    ConicalGradient cg{};
    if (fill.conicalGradient(&cg) && cg.stopCount > 0) {
      inst.params[0] = 3.f;
      inst.gradient[0] = cg.center.x;
      inst.gradient[1] = cg.center.y;
      inst.gradient[2] = cg.startAngleRadians;
      writeStops(cg);
      return;
    }
    putColor(inst.fill0, Colors::transparent);
    inst.params[1] = 1.f;
  }
  void createCommandObjects() {
    VulkanCommandObjectsContext context{
        .device = device_,
        .queueFamily = queueFamily_,
        .commandPool = commandPool_,
        .commandBuffers = commandBuffers_,
        .imageAvailable = imageAvailable_,
        .frameFences = frameFences_,
    };
    pipelines_.createCommandObjects(context);
  }

  void recoverResetFrameFence() {
    VulkanResetFrameFenceContext context{
        .device = device_,
        .resetFrameFenceIndex = resetFrameFenceIndex_,
        .noResetFrameFence = kNoResetFrameFence,
        .frameFences = frameFences_,
        .imageInFlightFences = imageInFlightFences_,
    };
    pipelines_.recoverResetFrameFence(context);
  }

  SharedVulkanCore::Resources &resources() {
    if (!shared_) {
      throw std::runtime_error("Vulkan shared resources are unavailable");
    }
    return shared_->resources;
  }

  SharedVulkanCore::Resources const &resources() const {
    if (!shared_) {
      throw std::runtime_error("Vulkan shared resources are unavailable");
    }
    return shared_->resources;
  }

  void ensureSharedResources() {
    auto &res = resources();
    VkFormat const format = surfaceFormat_.format == VK_FORMAT_UNDEFINED ? VK_FORMAT_B8G8R8A8_UNORM
                                                                         : surfaceFormat_.format;
    VkFormat const backdropFormat = backdropRenderTargetFormat();
    if (res.initialized) {
      if (res.renderFormat != format || res.backdropRenderFormat != backdropFormat) {
        throw std::runtime_error("Shared Vulkan resources cannot be reused with a different surface format");
      }
      return;
    }
    res.renderFormat = format;
    res.backdropRenderFormat = backdropFormat;
    pipelines_.createDescriptors(device_, res);
    pipelines_.createSampler(device_, res);
    createPipelineCache(*shared_);
    pipelines_.createPipelines(device_, res);
    createAtlas();
    res.initialized = true;
  }

  void chooseSurfaceFormat() {
    surfaceFormat_ = pipelines_.chooseSurfaceFormat(physical_, surface_);
  }

  void markFrameFenceComplete(std::size_t index) {
    swapchainController_.markFrameFenceComplete(frameFenceSubmitGenerations_,
                                                frameFenceCompleteGenerations_,
                                                index);
  }

  void markFrameFenceComplete(VkFence fence) {
    swapchainController_.markFrameFenceComplete(frameFences_,
                                                frameFenceSubmitGenerations_,
                                                frameFenceCompleteGenerations_,
                                                fence);
  }

  void markFrameFenceSubmitted(std::size_t index) {
    swapchainController_.markFrameFenceSubmitted(frameFenceSubmitGenerations_, index);
  }

  VkFence createPresentFence(bool signaled, char const* label) {
    return swapchainController_.createPresentFence(device_, signaled, label);
  }

  void retireSwapchains(bool force) {
    swapchainController_.retireSwapchains(device_,
                                          retiredSwapchains_,
                                          frameFenceCompleteGenerations_,
                                          force);
  }

  void retireSwapchainResources(VkSwapchainKHR swapchain,
                                std::vector<VkImageView> views,
                                std::vector<VkSemaphore> renderFinished,
                                std::vector<VkFence> presentFences) {
    swapchainController_.retireSwapchainResources(retiredSwapchains_,
                                                  frameFenceSubmitGenerations_,
                                                  swapchain,
                                                  std::move(views),
                                                  std::move(renderFinished),
                                                  std::move(presentFences));
  }

  void retirePresentFence(VkFence fence) {
    swapchainController_.retirePresentFence(retiredSwapchains_, fence);
  }

  void recreateSwapchain() {
    if (!device_) {
      return;
    }
    VulkanSwapchainRecreateContext context{
        .device = device_,
        .physical = physical_,
        .surface = surface_,
        .shared = shared_,
        .handle = handle_,
        .framebufferWidth = framebufferWidth_,
        .framebufferHeight = framebufferHeight_,
        .swapchainTargetWidth = swapchainTargetWidth_,
        .swapchainTargetHeight = swapchainTargetHeight_,
        .surfaceFormat = surfaceFormat_,
        .forceFifoPresentMode = vulkanForceFifoPresentMode(),
        .presentFencesEnabled = vulkanPresentFencesEnabled(),
        .presentFenceRuntimeDisabled = presentFenceRuntimeDisabled_,
        .transparentSurface = transparentSurface_,
        .swapchain = swapchain_,
        .presentMode = presentMode_,
        .swapExtent = swapExtent_,
        .swapchainImages = swapchainImages_,
        .swapchainViews = swapchainViews_,
        .imageInFlightFences = imageInFlightFences_,
        .imageRenderFinished = imageRenderFinished_,
        .imagePresentFences = imagePresentFences_,
        .retiredSwapchains = retiredSwapchains_,
        .frameFenceSubmitGenerations = frameFenceSubmitGenerations_,
    };
    swapchainController_.recreate(context);
    swapchainDirty_ = false;
  }

  void destroySwapchain() {
    VulkanSwapchainDestroyContext context{
        .device = device_,
        .swapchain = swapchain_,
        .swapchainImages = swapchainImages_,
        .swapchainViews = swapchainViews_,
        .imageInFlightFences = imageInFlightFences_,
        .imageRenderFinished = imageRenderFinished_,
        .imagePresentFences = imagePresentFences_,
        .retiredSwapchains = retiredSwapchains_,
        .frameFenceSubmitGenerations = frameFenceSubmitGenerations_,
        .frameFenceCompleteGenerations = frameFenceCompleteGenerations_,
    };
    swapchainController_.destroy(context);
  }

  VulkanGlyphSlot const* glyphSlot(std::uint32_t fontId, std::uint32_t glyphId, float fontSize) {
    return glyphAtlas_.glyphSlot(resources(), dpiScaleY_, fontId, glyphId, fontSize);
  }

  void ensureAtlasDescriptor() {
    ensureTextureDescriptor(resources().atlas);
  }

  void queueAtlasUploadIfNeeded() {
    auto &res = resources();
    if (!glyphAtlas_.atlasUploadNeeded(res)) {
      return;
    }
    if (!res.atlas.image ||
        res.atlasTextureWidth != res.atlas.width ||
        res.atlasTextureHeight != res.atlas.height) {
      int const width = res.atlas.width;
      int const height = res.atlas.height;
      VkFormat const format = res.atlas.format == VK_FORMAT_UNDEFINED
                                  ? VK_FORMAT_R8G8B8A8_UNORM
                                  : res.atlas.format;
      destroyTexture(res.atlas);
      createTexture(res.atlas, width, height, format);
      res.atlasTextureWidth = width;
      res.atlasTextureHeight = height;
      ensureTextureDescriptor(res.atlas);
    }
    queueTextureUpload(res.atlas, res.atlasPixels.data());
    res.atlasDirty = false;
  }

  void createAtlas() {
    auto &res = resources();
    res.atlas.width = 2048;
    res.atlas.height = 2048;
    res.atlasPixels.assign(static_cast<std::size_t>(res.atlas.width) * res.atlas.height, Rgba{255, 255, 255, 0});
    createTexture(res.atlas,
                  res.atlas.width,
                  res.atlas.height,
                  VK_FORMAT_R8G8B8A8_UNORM);
    res.atlasTextureWidth = res.atlas.width;
    res.atlasTextureHeight = res.atlas.height;
    ensureTextureDescriptor(res.atlas);
  }

  void appendPath(Path const &path, FillStyle const &fill, StrokeStyle const &stroke) {
    if (path.isEmpty())
      return;
    RecordingTarget target = recordingTarget();
    PathCacheKey const cacheKey{
        .pathHash = path.contentHash(),
        .styleHash = hashFill(fill) ^ (hashStroke(stroke) + 0x9e3779b97f4a7c15ULL) ^
                     (hashTransform(state_.transform, state_.opacity) << 1u) ^
                     (currentRoundedClipHash() << 2u),
        .viewportW = width_,
        .viewportH = height_,
    };
    if (auto it = pathCache_.find(cacheKey); it != pathCache_.end()) {
      pathCacheLru_.splice(pathCacheLru_.end(), pathCacheLru_, it->second.lruIt);
      std::uint32_t const firstVertex = static_cast<std::uint32_t>(target.pathVerts.size());
      target.pathVerts.insert(target.pathVerts.end(), it->second.vertices.begin(), it->second.vertices.end());
      if (!it->second.vertices.empty()) {
        appendSignedDrawOp(target,
                           makeDrawOp(DrawOp::Kind::Path,
                                      nullptr,
                                      firstVertex,
                                      static_cast<std::uint32_t>(it->second.vertices.size())));
      }
      return;
    }
    auto subpaths = PathFlattener::flattenSubpaths(path);
    if (subpaths.empty())
      return;
    std::uint32_t const firstVertex = static_cast<std::uint32_t>(target.pathVerts.size());
    for (auto &sp : subpaths) {
      for (Point &p : sp)
        p = state_.transform.apply(p);
    }
    Rect const bounds = boundsOfSubpaths(subpaths);
    auto append = [&](TessellatedPath &&tess, FillStyle const *gradientSource = nullptr) {
      if (gradientSource) {
        for (PathVertex const &vertex : tess.vertices) {
          VulkanPathVertex out = makeVulkanPathVertex(vertex, gradientSource, bounds, state_.opacity);
          applyCurrentRoundedClip(out);
          target.pathVerts.push_back(out);
        }
      } else {
        for (PathVertex const &vertex : tess.vertices) {
          VulkanPathVertex out = makeVulkanPathVertex(vertex, nullptr, bounds, 1.f);
          applyCurrentRoundedClip(out);
          target.pathVerts.push_back(out);
        }
      }
    };
    auto const tessellateStart = std::chrono::steady_clock::now();
    bool tessellated = false;
    if (!fill.isNone()) {
      Color fc{};
      if (representativeFillColor(fill, &fc)) {
        fc.a *= state_.opacity;
        std::vector<std::vector<Point>> nonempty;
        for (auto const &sp : subpaths) {
          if (sp.size() >= 3)
            nonempty.push_back(sp);
        }
        if (!nonempty.empty()) {
          tessellated = true;
          append(PathFlattener::tessellateFillContours(nonempty, fc, static_cast<float>(width_),
                                                       static_cast<float>(height_),
                                                       PathFlattener::tessWindingFromFillRule(fill.fillRule)),
                 &fill);
        }
      }
    }
    if (!stroke.isNone()) {
      Color sc{};
      if (stroke.solidColor(&sc)) {
        sc.a *= state_.opacity;
        float const sx = std::hypot(state_.transform.m[0], state_.transform.m[1]);
        float const sy = std::hypot(state_.transform.m[3], state_.transform.m[4]);
        float const s = (sx > 0.f || sy > 0.f) ? (sx + sy) * 0.5f : 1.f;
        for (auto const &sp : subpaths) {
          if (sp.size() >= 2) {
            tessellated = true;
            append(PathFlattener::tessellateStroke(sp, stroke.width * s, sc, static_cast<float>(width_),
                                                   static_cast<float>(height_), stroke.join, stroke.cap));
          }
        }
      }
    }
    if (tessellated) {
      debug::perf::recordVulkanPathTessellation(std::chrono::steady_clock::now() - tessellateStart);
    }
    std::uint32_t const vertexCount = static_cast<std::uint32_t>(target.pathVerts.size()) - firstVertex;
    if (vertexCount > 0) {
      std::vector<VulkanPathVertex> cached(target.pathVerts.begin() + firstVertex, target.pathVerts.end());
      pathCacheLru_.push_back(cacheKey);
      auto lruIt = std::prev(pathCacheLru_.end());
      auto [it, inserted] = pathCache_.emplace(cacheKey, CachedPath{std::move(cached), lruIt});
      if (inserted) {
        cachedPathVertexCount_ += it->second.vertices.size();
      } else {
        pathCacheLru_.erase(lruIt);
      }
      trimPathCache();
      appendSignedDrawOp(target, makeDrawOp(DrawOp::Kind::Path, nullptr, firstVertex, vertexCount));
    }
  }

  void trimPathCache() {
    constexpr std::size_t kMaxCachedPathVertices = 500'000;
    while (cachedPathVertexCount_ > kMaxCachedPathVertices && !pathCache_.empty()) {
      if (pathCacheLru_.empty()) {
        pathCache_.clear();
        cachedPathVertexCount_ = 0;
        return;
      }
      PathCacheKey const key = pathCacheLru_.front();
      auto it = pathCache_.find(key);
      if (it != pathCache_.end()) {
        cachedPathVertexCount_ -= it->second.vertices.size();
        pathCache_.erase(it);
      }
      pathCacheLru_.pop_front();
    }
  }

  void uploadFrameBuffers() {
    FrameGeometryResources& geometry = frameGeometryResources();
    ensureBuffer(geometry.rectBuffer,
                 std::max<VkDeviceSize>(sizeof(RectInstance), rects_.size() * sizeof(RectInstance)),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    upload(geometry.rectBuffer, rects_.data(), rects_.size() * sizeof(RectInstance));
    ensureStorageDescriptor(geometry.rectDescriptorSet, resources().rectDescriptorLayout, geometry.rectBuffer);
    ensureBuffer(geometry.calloutBuffer,
                 std::max<VkDeviceSize>(sizeof(CalloutInstance), callouts_.size() * sizeof(CalloutInstance)),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    upload(geometry.calloutBuffer, callouts_.data(), callouts_.size() * sizeof(CalloutInstance));
    ensureStorageDescriptor(geometry.calloutDescriptorSet, resources().rectDescriptorLayout, geometry.calloutBuffer);
    ensureBuffer(geometry.quadBuffer,
                 std::max<VkDeviceSize>(sizeof(QuadInstance), quads_.size() * sizeof(QuadInstance)),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    upload(geometry.quadBuffer, quads_.data(), quads_.size() * sizeof(QuadInstance));
    ensureStorageDescriptor(geometry.quadDescriptorSet, resources().quadDescriptorLayout, geometry.quadBuffer);
    ensureBuffer(geometry.pathBuffer,
                 std::max<VkDeviceSize>(sizeof(VulkanPathVertex), pathVerts_.size() * sizeof(VulkanPathVertex)),
                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    upload(geometry.pathBuffer, pathVerts_.data(), pathVerts_.size() * sizeof(VulkanPathVertex));
  }

  FrameGeometryResources& frameGeometryResources() {
    return frameGeometry_[currentFrame_ % frameGeometry_.size()];
  }

  FrameGeometryResources const& frameGeometryResources() const {
    return frameGeometry_[currentFrame_ % frameGeometry_.size()];
  }

  void ensureBuffer(Buffer &buffer, VkDeviceSize size, VkBufferUsageFlags usage) {
    if (buffer.buffer && buffer.capacity >= size)
      return;
    destroyBuffer(buffer);
    buffer.capacity = std::max<VkDeviceSize>(size, 1024);
    auto info = vkStructure<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
    info.size = buffer.capacity;
    info.usage = usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo allocationInfo{};
    vkCheck(vmaCreateBuffer(allocator_, &info, &allocation, &buffer.buffer, &buffer.allocation, &allocationInfo),
            "vmaCreateBuffer");
    buffer.mapped = allocationInfo.pMappedData;
  }

  void upload(Buffer &buffer, void const *data, std::size_t size) {
    if (!size)
      return;
    if (buffer.mapped) {
      std::memcpy(buffer.mapped, data, size);
      vkCheck(vmaFlushAllocation(allocator_, buffer.allocation, 0, size), "vmaFlushAllocation");
      return;
    }
    void *mapped = nullptr;
    vkCheck(vmaMapMemory(allocator_, buffer.allocation, &mapped), "vmaMapMemory");
    std::memcpy(mapped, data, size);
    vkCheck(vmaFlushAllocation(allocator_, buffer.allocation, 0, size), "vmaFlushAllocation");
    vmaUnmapMemory(allocator_, buffer.allocation);
  }

  void uploadRows(Buffer& buffer,
                  void const* data,
                  std::size_t rowBytes,
                  std::uint32_t rowCount,
                  std::size_t sourceBytesPerRow) {
    if (!data || rowBytes == 0 || rowCount == 0) {
      return;
    }
    std::size_t const totalBytes = rowBytes * static_cast<std::size_t>(rowCount);
    auto copyRows = [&](void* mapped) {
      auto* dst = static_cast<std::uint8_t*>(mapped);
      auto const* src = static_cast<std::uint8_t const*>(data);
      for (std::uint32_t row = 0; row < rowCount; ++row) {
        std::memcpy(dst + static_cast<std::size_t>(row) * rowBytes,
                    src + static_cast<std::size_t>(row) * sourceBytesPerRow,
                    rowBytes);
      }
    };
    if (buffer.mapped) {
      copyRows(buffer.mapped);
      vkCheck(vmaFlushAllocation(allocator_, buffer.allocation, 0, totalBytes), "vmaFlushAllocation");
      return;
    }
    void* mapped = nullptr;
    vkCheck(vmaMapMemory(allocator_, buffer.allocation, &mapped), "vmaMapMemory");
    copyRows(mapped);
    vkCheck(vmaFlushAllocation(allocator_, buffer.allocation, 0, totalBytes), "vmaFlushAllocation");
    vmaUnmapMemory(allocator_, buffer.allocation);
  }

  void destroyBuffer(Buffer &buffer) {
    if (buffer.buffer)
      vmaDestroyBuffer(allocator_, buffer.buffer, buffer.allocation);
    buffer = {};
  }

  void destroyStorageDescriptor(VkDescriptorSet& set) {
    if (set && resources().descriptorPool) {
      vkFreeDescriptorSets(device_, resources().descriptorPool, 1, &set);
    }
    set = VK_NULL_HANDLE;
  }

  void destroyFrameGeometryResources(FrameGeometryResources& geometry) {
    destroyStorageDescriptor(geometry.rectDescriptorSet);
    destroyStorageDescriptor(geometry.calloutDescriptorSet);
    destroyStorageDescriptor(geometry.quadDescriptorSet);
    destroyBuffer(geometry.pathBuffer);
    destroyBuffer(geometry.rectBuffer);
    destroyBuffer(geometry.calloutBuffer);
    destroyBuffer(geometry.quadBuffer);
  }

  static Buffer takeBuffer(Buffer& buffer) {
    Buffer out = buffer;
    buffer = {};
    return out;
  }

  void ensureStorageDescriptor(VkDescriptorSet &set, VkDescriptorSetLayout layout, Buffer const &buffer) {
    if (!set) {
      auto alloc = vkStructure<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
      alloc.descriptorPool = resources().descriptorPool;
      alloc.descriptorSetCount = 1;
      alloc.pSetLayouts = &layout;
      vkCheck(vkAllocateDescriptorSets(device_, &alloc, &set), "vkAllocateDescriptorSets");
    }
    VkDescriptorBufferInfo bi{buffer.buffer, 0, buffer.capacity};
    auto write = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &bi;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }

  void ensureBackdropSceneTarget() {
    std::uint32_t const downsample = backdropBlurDownsample();
    int const targetW = static_cast<int>(std::max(1u, (swapExtent_.width + downsample - 1u) / downsample));
    int const targetH = static_cast<int>(std::max(1u, (swapExtent_.height + downsample - 1u) / downsample));
    VkFormat const backdropFormat = backdropRenderTargetFormat();
    auto ensure = [&](Texture &texture) {
      if (texture.image && texture.width == targetW && texture.height == targetH && texture.format == backdropFormat) {
        return;
      }
      destroyTexture(texture);
      createRenderTargetTexture(texture, targetW, targetH, backdropFormat);
      clearBackdropBlurCache();
    };
    ensure(backdropSceneTexture_);
    ensure(backdropScratchTexture_);
    ensure(backdropBlurTexture_);
  }

  void clearBackdropBlurCache() {
    for (CachedBackdropBlur &entry : backdropBlurCache_) {
      destroyTexture(entry.texture);
    }
    backdropBlurCache_.clear();
  }

  std::uint32_t appendBackdropBlurQuad(float radiusPx, float axisX, float axisY) {
    QuadInstance q{};
    q.rect[0] = 0.f;
    q.rect[1] = 0.f;
    q.rect[2] = static_cast<float>(width_);
    q.rect[3] = static_cast<float>(height_);
    q.axisX[0] = 0.f;
    q.axisX[1] = 0.f;
    q.axisX[2] = static_cast<float>(width_);
    q.axisX[3] = 0.f;
    q.axisY[0] = 0.f;
    q.axisY[1] = static_cast<float>(height_);
    q.uv[0] = 0.f;
    q.uv[1] = 0.f;
    q.uv[2] = 1.f;
    q.uv[3] = 1.f;
    q.color[0] = q.color[1] = q.color[2] = q.color[3] = 0.f;
    q.radii[0] = radiusPx;
    q.radii[1] = axisX;
    q.radii[2] = axisY;
    q.radii[3] = 0.f;
    std::uint32_t const first = static_cast<std::uint32_t>(quads_.size());
    quads_.push_back(q);
    return first;
  }

  struct BackdropBlurRun {
    std::size_t start = 0;
    std::size_t end = 0;
    std::uint32_t horizontalQuad = 0;
    std::uint32_t verticalQuad = 0;
    Rect clip{};
  };

  Rect intersectRects(Rect a, Rect b) const {
    return lambdaui::intersectRects(a, b);
  }

  Rect quadBounds(std::uint32_t first, std::uint32_t count) const {
    Rect bounds = Rect::sharp(0.f, 0.f, 0.f, 0.f);
    bool hasBounds = false;
    std::uint32_t const end = std::min<std::uint32_t>(first + count, static_cast<std::uint32_t>(quads_.size()));
    for (std::uint32_t index = first; index < end; ++index) {
      QuadInstance const &q = quads_[index];
      Point const p00{q.axisX[0], q.axisX[1]};
      Point const p10{q.axisX[0] + q.axisX[2], q.axisX[1] + q.axisX[3]};
      Point const p01{q.axisX[0] + q.axisY[0], q.axisX[1] + q.axisY[1]};
      Point const p11{p10.x + q.axisY[0], p10.y + q.axisY[1]};
      float const x0 = std::min({p00.x, p10.x, p01.x, p11.x});
      float const y0 = std::min({p00.y, p10.y, p01.y, p11.y});
      float const x1 = std::max({p00.x, p10.x, p01.x, p11.x});
      float const y1 = std::max({p00.y, p10.y, p01.y, p11.y});
      Rect const next = Rect::sharp(x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0));
      bounds = hasBounds ? unionRects(bounds, next) : next;
      hasBounds = true;
    }
    return hasBounds ? bounds : Rect::sharp(0.f, 0.f, 0.f, 0.f);
  }

  Rect rectInstanceBounds(RectInstance const& r) const {
    Point const p00{r.axisX[0], r.axisX[1]};
    Point const p10{r.axisX[0] + r.axisX[2], r.axisX[1] + r.axisX[3]};
    Point const p01{r.axisX[0] + r.axisY[0], r.axisX[1] + r.axisY[1]};
    Point const p11{p10.x + r.axisY[0], p10.y + r.axisY[1]};
    float const x0 = std::min({p00.x, p10.x, p01.x, p11.x});
    float const y0 = std::min({p00.y, p10.y, p01.y, p11.y});
    float const x1 = std::max({p00.x, p10.x, p01.x, p11.x});
    float const y1 = std::max({p00.y, p10.y, p01.y, p11.y});
    return Rect::sharp(x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0));
  }

  Rect rectBounds(std::uint32_t first, std::uint32_t count) const {
    Rect bounds = Rect::sharp(0.f, 0.f, 0.f, 0.f);
    bool hasBounds = false;
    std::uint32_t const end = std::min<std::uint32_t>(first + count, static_cast<std::uint32_t>(rects_.size()));
    for (std::uint32_t index = first; index < end; ++index) {
      Rect const next = rectInstanceBounds(rects_[index]);
      bounds = hasBounds ? unionRects(bounds, next) : next;
      hasBounds = true;
    }
    return hasBounds ? bounds : Rect::sharp(0.f, 0.f, 0.f, 0.f);
  }

  Rect calloutBounds(std::uint32_t first, std::uint32_t count) const {
    Rect bounds = Rect::sharp(0.f, 0.f, 0.f, 0.f);
    bool hasBounds = false;
    std::uint32_t const end = std::min<std::uint32_t>(first + count, static_cast<std::uint32_t>(callouts_.size()));
    for (std::uint32_t index = first; index < end; ++index) {
      CalloutInstance const& c = callouts_[index];
      RectInstance r{};
      r.axisX[0] = c.axisX[0];
      r.axisX[1] = c.axisX[1];
      r.axisX[2] = c.axisX[2];
      r.axisX[3] = c.axisX[3];
      r.axisY[0] = c.axisY[0];
      r.axisY[1] = c.axisY[1];
      Rect const next = rectInstanceBounds(r);
      bounds = hasBounds ? unionRects(bounds, next) : next;
      hasBounds = true;
    }
    return hasBounds ? bounds : Rect::sharp(0.f, 0.f, 0.f, 0.f);
  }

  Rect pathBounds(std::uint32_t first, std::uint32_t count) const {
    std::uint32_t const end = std::min<std::uint32_t>(first + count, static_cast<std::uint32_t>(pathVerts_.size()));
    if (first >= end) {
      return Rect::sharp(0.f, 0.f, 0.f, 0.f);
    }
    float x0 = pathVerts_[first].x;
    float y0 = pathVerts_[first].y;
    float x1 = x0;
    float y1 = y0;
    for (std::uint32_t index = first + 1; index < end; ++index) {
      x0 = std::min(x0, pathVerts_[index].x);
      y0 = std::min(y0, pathVerts_[index].y);
      x1 = std::max(x1, pathVerts_[index].x);
      y1 = std::max(y1, pathVerts_[index].y);
    }
    return Rect::sharp(x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0));
  }

  bool drawOpBounds(DrawOp const& op, Rect& bounds) const {
    if (op.externalStorageDescriptor || op.externalVertexBuffer) {
      return false;
    }
    switch (op.kind) {
    case DrawOp::Kind::Rect:
      bounds = rectBounds(op.first, op.count);
      return op.first + op.count <= rects_.size();
    case DrawOp::Kind::Callout:
      bounds = calloutBounds(op.first, op.count);
      return op.first + op.count <= callouts_.size();
    case DrawOp::Kind::Path:
      bounds = pathBounds(op.first, op.count);
      return op.first + op.count <= pathVerts_.size();
    case DrawOp::Kind::Image:
    case DrawOp::Kind::BackdropBlur:
      bounds = quadBounds(op.first, op.count);
      return op.first + op.count <= quads_.size();
    }
    return false;
  }

  bool drawOpAffectsBackdropClip(DrawOp const& op, Rect const& clip) const {
    Rect bounds{};
    if (!drawOpBounds(op, bounds)) {
      return true;
    }
    Rect const clipped = intersectRects(bounds, op.clip);
    return clipped.width > 0.f && clipped.height > 0.f && clipped.intersects(clip);
  }

  Rect backdropBlurRunClip(std::size_t start, std::size_t end, float radiusPx) const {
    Rect bounds = Rect::sharp(0.f, 0.f, 0.f, 0.f);
    bool hasBounds = false;
    std::size_t const opEnd = std::min(end, ops_.size());
    for (std::size_t index = std::min(start, opEnd); index < opEnd; ++index) {
      DrawOp const &op = ops_[index];
      if (op.kind != DrawOp::Kind::BackdropBlur) continue;
      Rect const blurBounds = op.hasBlurCacheClip ? op.blurCacheClip : quadBounds(op.first, op.count);
      Rect opBounds = intersectRects(blurBounds, op.clip);
      if (opBounds.width <= 0.f || opBounds.height <= 0.f) continue;
      bounds = hasBounds ? unionRects(bounds, opBounds) : opBounds;
      hasBounds = true;
    }
    if (!hasBounds) {
      return Rect::sharp(0.f, 0.f, static_cast<float>(width_), static_cast<float>(height_));
    }
    float const scale = std::max(0.001f, std::max(dpiScaleX_, dpiScaleY_));
    float const pad = std::ceil(radiusPx / scale) + 2.f;
    Rect const padded = Rect::sharp(bounds.x - pad,
                                    bounds.y - pad,
                                    bounds.width + pad * 2.f,
                                    bounds.height + pad * 2.f);
    return intersectRects(padded, Rect::sharp(0.f, 0.f, static_cast<float>(width_), static_cast<float>(height_)));
  }

  std::size_t nextBackdropBlurOp(std::size_t start = 0) const {
    std::size_t const begin = std::min(start, ops_.size());
    auto const it = std::find_if(ops_.begin() + static_cast<std::ptrdiff_t>(begin),
                                 ops_.end(),
                                 [](DrawOp const &op) {
      return op.kind == DrawOp::Kind::BackdropBlur;
    });
    return it == ops_.end() ? ops_.size() : static_cast<std::size_t>(std::distance(ops_.begin(), it));
  }

  std::size_t backdropBlurRunEnd(std::size_t start) const {
    std::size_t end = start;
    while (end < ops_.size() && ops_[end].kind == DrawOp::Kind::BackdropBlur) {
      ++end;
    }
    return end;
  }

  std::pair<std::uint64_t, std::uint64_t> backdropBlurRunOpCounts(std::size_t start, std::size_t end) const {
    std::uint64_t ops = 0;
    std::uint64_t quads = 0;
    std::size_t const opEnd = std::min(end, ops_.size());
    for (std::size_t index = std::min(start, opEnd); index < opEnd; ++index) {
      DrawOp const& op = ops_[index];
      if (op.kind != DrawOp::Kind::BackdropBlur) {
        continue;
      }
      ++ops;
      quads += op.count;
    }
    return {ops, quads};
  }

  float maxBackdropBlurRadius(std::size_t start, std::size_t end) const {
    float radius = 0.f;
    std::size_t const opEnd = std::min(end, ops_.size());
    for (std::size_t index = std::min(start, opEnd); index < opEnd; ++index) {
      DrawOp const &op = ops_[index];
      if (op.kind == DrawOp::Kind::BackdropBlur) {
        radius = std::max(radius, op.blurRadius);
      }
    }
    return radius;
  }

  VkExtent2D backdropTextureExtent() const {
    return VkExtent2D{
        static_cast<std::uint32_t>(std::max(1, backdropBlurTexture_.width)),
        static_cast<std::uint32_t>(std::max(1, backdropBlurTexture_.height)),
    };
  }

  std::uint32_t backdropBlurDownsample() const {
    float const scale = std::max(dpiScaleX_, dpiScaleY_);
    float const scaleFactor = std::max(1.f, scale);
    long const scaledDownsample =
        std::lround(static_cast<float>(backdropBlurBaseDownsample_) * scaleFactor);
    return static_cast<std::uint32_t>(
        std::clamp<long>(scaledDownsample, kMinBackdropBlurBaseDownsample, kMaxBackdropBlurDownsample));
  }

  void hashTextureReference(std::uint64_t &h, Texture const *texture) const {
    auto image = reinterpret_cast<std::uintptr_t>(texture ? texture->image : VK_NULL_HANDLE);
    auto view = reinterpret_cast<std::uintptr_t>(texture ? texture->view : VK_NULL_HANDLE);
    auto descriptor = reinterpret_cast<std::uintptr_t>(texture ? texture->descriptor : VK_NULL_HANDLE);
    hashValue(h, image);
    hashValue(h, view);
    hashValue(h, descriptor);
    if (texture) {
      hashValue(h, texture->format);
      hashValue(h, texture->layout);
      hashValue(h, texture->width);
      hashValue(h, texture->height);
      hashValue(h, texture->contentGeneration);
    }
  }

  template <typename Rects, typename Quads, typename PathVerts>
  std::uint64_t drawOpGeometrySignature(DrawOp const &op,
                                        Rects const &rects,
                                        Quads const &quads,
                                        PathVerts const &pathVerts) const {
    std::uint64_t h = 14695981039346656037ULL;
    hashValue(h, op.kind);
    hashValue(h, op.count);
    hashValue(h, op.clip);
    hashValue(h, op.blurRadius);
    hashValue(h, op.blurCacheClip);
    hashValue(h, op.hasBlurCacheClip);
    hashValue(h, reinterpret_cast<std::uintptr_t>(op.externalStorageDescriptor));
    hashValue(h, reinterpret_cast<std::uintptr_t>(op.externalVertexBuffer));
    hashValue(h, op.externalTranslationX);
    hashValue(h, op.externalTranslationY);
    hashValue(h, op.premultipliedAlpha);
    hashValue(h, reinterpret_cast<std::uintptr_t>(op.sourceImage));
    hashTextureReference(h, op.texture);

    std::uint32_t const end = op.first + op.count;
    switch (op.kind) {
    case DrawOp::Kind::Rect:
      for (std::uint32_t index = op.first; index < end && index < rects.size(); ++index) {
        hashValue(h, rects[index]);
      }
      break;
    case DrawOp::Kind::Callout:
      for (std::uint32_t index = op.first; index < end && index < callouts_.size(); ++index) {
        hashValue(h, callouts_[index]);
      }
      break;
    case DrawOp::Kind::Path:
      for (std::uint32_t index = op.first; index < end && index < pathVerts.size(); ++index) {
        hashValue(h, pathVerts[index]);
      }
      break;
    case DrawOp::Kind::Image:
    case DrawOp::Kind::BackdropBlur:
      for (std::uint32_t index = op.first; index < end && index < quads.size(); ++index) {
        hashValue(h, quads[index]);
      }
      break;
    }
    return nonZeroSignature(h);
  }

  void prepareRecordedGeometrySignatures(VulkanFrameRecorder &recorded) const {
    if (recorded.geometrySignaturesPrepared) {
      return;
    }
    for (DrawOp &op : recorded.ops) {
      op.geometrySignature =
          drawOpGeometrySignature(op, recorded.rects, recorded.quads, recorded.pathVerts);
    }
    recorded.geometrySignaturesPrepared = true;
  }

  void hashDrawOpGeometry(std::uint64_t &h, DrawOp const &op) const {
    if (op.geometrySignature != 0) {
      hashValue(h, op.geometrySignature);
      return;
    }
    hashValue(h, drawOpGeometrySignature(op, rects_, quads_, pathVerts_));
  }

  std::uint64_t backdropBlurSignature(BackdropBlurRun const &run, std::size_t runIndex) const {
    std::uint64_t h = 14695981039346656037ULL;
    hashValue(h, runIndex);
    hashValue(h, width_);
    hashValue(h, height_);
    hashValue(h, framebufferWidth_);
    hashValue(h, framebufferHeight_);
    hashValue(h, swapExtent_.width);
    hashValue(h, swapExtent_.height);
    hashValue(h, clearColor_);
    hashValue(h, run.clip);
    hashValue(h, kBackdropBlurIterations);
    hashValue(h, backdropBlurDownsample());
    hashValue(h, kBackdropBlurRadiusBoost);
    if (run.horizontalQuad < quads_.size()) {
      hashValue(h, quads_[run.horizontalQuad]);
    }
    if (run.verticalQuad < quads_.size()) {
      hashValue(h, quads_[run.verticalQuad]);
    }
    std::size_t const opEnd = std::min(run.start, ops_.size());
    std::uint64_t affectingOps = 0;
    for (std::size_t index = 0; index < opEnd; ++index) {
      DrawOp const& op = ops_[index];
      if (!drawOpAffectsBackdropClip(op, run.clip)) {
        continue;
      }
      hashDrawOpGeometry(h, op);
      ++affectingOps;
    }
    hashValue(h, affectingOps);
    return h;
  }

  std::uint64_t renderTargetFrameSignature() const {
    std::uint64_t h = 14695981039346656037ULL;
    hashValue(h, reinterpret_cast<std::uintptr_t>(targetSpec_.image));
    hashValue(h, reinterpret_cast<std::uintptr_t>(targetSpec_.view));
    hashValue(h, targetSpec_.format);
    hashValue(h, targetSpec_.width);
    hashValue(h, targetSpec_.height);
    hashValue(h, targetSpec_.initialLayout);
    hashValue(h, targetSpec_.finalLayout);
    hashValue(h, targetSpec_.preserveContents);
    hashValue(h, width_);
    hashValue(h, height_);
    hashValue(h, framebufferWidth_);
    hashValue(h, framebufferHeight_);
    hashValue(h, dpiScaleX_);
    hashValue(h, dpiScaleY_);
    hashValue(h, backdropBlurBaseDownsample_);
    hashValue(h, clearColor_);
    hashValue(h, resources().atlasGeneration);
    hashValue(h, resources().atlasDirty);
    hashValue(h, ops_.size());
    for (DrawOp const &op : ops_) {
      hashDrawOpGeometry(h, op);
    }
    return h;
  }

  CachedBackdropBlur &ensureBackdropBlurCacheEntry(std::size_t index) {
    if (backdropBlurCache_.size() <= index) {
      backdropBlurCache_.resize(index + 1);
    }
    CachedBackdropBlur &entry = backdropBlurCache_[index];
    VkExtent2D const extent = backdropTextureExtent();
    VkFormat const backdropFormat = backdropRenderTargetFormat();
    if (!entry.texture.image ||
        entry.texture.width != static_cast<int>(extent.width) ||
        entry.texture.height != static_cast<int>(extent.height) ||
        entry.texture.format != backdropFormat) {
      destroyTexture(entry.texture);
      createRenderTargetTexture(entry.texture,
                                static_cast<int>(extent.width),
                                static_cast<int>(extent.height),
                                backdropFormat);
      entry.valid = false;
      entry.signature = 0;
    }
    return entry;
  }

  std::vector<BackdropBlurRun> prepareBackdropBlurRuns() {
    std::vector<BackdropBlurRun> runs;
    std::size_t cursor = 0;
    while (cursor < ops_.size()) {
      std::size_t const start = nextBackdropBlurOp(cursor);
      if (start >= ops_.size()) break;
      if (runs.empty()) {
        ensureBackdropSceneTarget();
      }
      std::size_t const end = backdropBlurRunEnd(start);
      auto const [opCount, quadCount] = backdropBlurRunOpCounts(start, end);
      float const maxRadius = maxBackdropBlurRadius(start, end);
      float const effectiveRadius = maxRadius * kBackdropBlurRadiusBoost;
      float const blurRadius = (effectiveRadius / static_cast<float>(backdropBlurDownsample())) /
                               std::sqrt(static_cast<float>(kBackdropBlurIterations));
      BackdropBlurRun run{
          .start = start,
          .end = end,
          .horizontalQuad = appendBackdropBlurQuad(blurRadius, 1.f, 0.f),
          .verticalQuad = appendBackdropBlurQuad(blurRadius, 0.f, 1.f),
          .clip = backdropBlurRunClip(start, end, effectiveRadius),
      };
      debug::perf::recordBackdropBlurPreparedRun(opCount, quadCount);
      runs.push_back(run);
      cursor = end;
    }
    return runs;
  }

  VulkanDrawCommandContext drawCommandContext() const {
    return VulkanDrawCommandContext{
        .viewportWidth = static_cast<float>(width_),
        .viewportHeight = static_cast<float>(height_),
        .resources = &resources(),
        .geometry = &frameGeometryResources(),
    };
  }

  struct PixelRect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool valid() const { return width > 0 && height > 0; }
  };

  PixelRect pixelRectForLogicalClip(Rect clip) const {
    return pixelRectForLogicalClip(clip,
                                   VkExtent2D{
                                       static_cast<std::uint32_t>(std::max(1, framebufferWidth_)),
                                       static_cast<std::uint32_t>(std::max(1, framebufferHeight_)),
                                   });
  }

  VkExtent2D currentFramebufferExtent() const {
    return VkExtent2D{
        static_cast<std::uint32_t>(std::max(1, framebufferWidth_)),
        static_cast<std::uint32_t>(std::max(1, framebufferHeight_)),
    };
  }

  PixelRect currentFramebufferPixelRect() const {
    VkExtent2D const extent = currentFramebufferExtent();
    return PixelRect{0u, 0u, extent.width, extent.height};
  }

  PixelRect pixelRectForLogicalClip(Rect clip, VkExtent2D extent) const {
    float const renderWidth = static_cast<float>(std::max(1u, extent.width));
    float const renderHeight = static_cast<float>(std::max(1u, extent.height));
    float const scaleX = renderWidth / std::max(1.f, static_cast<float>(width_));
    float const scaleY = renderHeight / std::max(1.f, static_cast<float>(height_));
    float const x0f = std::clamp(std::floor(clip.x * scaleX), 0.f, renderWidth);
    float const y0f = std::clamp(std::floor(clip.y * scaleY), 0.f, renderHeight);
    float const x1f = std::clamp(std::ceil((clip.x + clip.width) * scaleX), 0.f, renderWidth);
    float const y1f = std::clamp(std::ceil((clip.y + clip.height) * scaleY), 0.f, renderHeight);
    std::uint32_t const x0 = static_cast<std::uint32_t>(x0f);
    std::uint32_t const y0 = static_cast<std::uint32_t>(y0f);
    std::uint32_t const x1 = static_cast<std::uint32_t>(x1f);
    std::uint32_t const y1 = static_cast<std::uint32_t>(y1f);
    return PixelRect{x0, y0, x1 > x0 ? x1 - x0 : 0u, y1 > y0 ? y1 - y0 : 0u};
  }

  void drawBackdropBlurPass(VkCommandBuffer commandBuffer,
                            VulkanCommandState &state,
                            Texture *texture,
                            std::uint32_t first,
                            Rect const &clip,
                            VkExtent2D targetExtent) {
    setViewportScissor(commandBuffer, clip, targetExtent);
    drawVulkanBackdropBlurPass(commandBuffer, state, drawCommandContext(), texture, first);
  }

  void copyTargetToBackdropScene(VkCommandBuffer commandBuffer,
                                 VkImage targetImage,
                                 VkImageLayout &targetLayout,
                                 Rect const &clip) {
    transition(commandBuffer, targetImage, targetLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    targetLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    transition(commandBuffer, backdropSceneTexture_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

    PixelRect const srcRect = pixelRectForLogicalClip(clip);
    VkExtent2D const backdropExtent{
        static_cast<std::uint32_t>(std::max(1, backdropSceneTexture_.width)),
        static_cast<std::uint32_t>(std::max(1, backdropSceneTexture_.height)),
    };
    PixelRect const dstRect = pixelRectForLogicalClip(clip, backdropExtent);
    if (!srcRect.valid() || !dstRect.valid()) {
      transition(commandBuffer, backdropSceneTexture_, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
      ensureTextureDescriptor(backdropSceneTexture_);
      return;
    }
    auto blit = vkStructure<VkImageBlit2>(VK_STRUCTURE_TYPE_IMAGE_BLIT_2);
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[0] = {static_cast<std::int32_t>(srcRect.x), static_cast<std::int32_t>(srcRect.y), 0};
    blit.srcOffsets[1] = {static_cast<std::int32_t>(srcRect.x + srcRect.width),
                          static_cast<std::int32_t>(srcRect.y + srcRect.height),
                          1};
    blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.dstSubresource.layerCount = 1;
    blit.dstOffsets[0] = {static_cast<std::int32_t>(dstRect.x), static_cast<std::int32_t>(dstRect.y), 0};
    blit.dstOffsets[1] = {static_cast<std::int32_t>(dstRect.x + dstRect.width),
                          static_cast<std::int32_t>(dstRect.y + dstRect.height),
                          1};
    auto blitInfo = vkStructure<VkBlitImageInfo2>(VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2);
    blitInfo.srcImage = targetImage;
    blitInfo.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    blitInfo.dstImage = backdropSceneTexture_.image;
    blitInfo.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    blitInfo.regionCount = 1;
    blitInfo.pRegions = &blit;
    blitInfo.filter = VK_FILTER_LINEAR;
    vkCmdBlitImage2(commandBuffer, &blitInfo);

    transition(commandBuffer, backdropSceneTexture_, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
    ensureTextureDescriptor(backdropSceneTexture_);
  }

  Texture *blurBackdropScene(VkCommandBuffer commandBuffer,
                             BackdropBlurRun const &run,
                             VkClearValue const &clear,
                             CachedBackdropBlur &cacheEntry,
                             std::uint64_t signature) {
    Texture *blurSource = &backdropSceneTexture_;
    Texture *blurDestination = &cacheEntry.texture;
    VkExtent2D const blurExtent = backdropTextureExtent();
    PixelRect const renderArea = pixelRectForLogicalClip(run.clip, blurExtent);
    if (!renderArea.valid()) {
      cacheEntry.valid = false;
      return nullptr;
    }
    PixelRect const sourceArea = pixelRectForLogicalClip(run.clip);
    std::uint64_t const blurPixels = static_cast<std::uint64_t>(renderArea.width) * renderArea.height;
    std::uint64_t const copyPixels = static_cast<std::uint64_t>(sourceArea.width) * sourceArea.height;
    debug::perf::recordBackdropBlurRun(copyPixels,
                                       blurPixels * 2u * static_cast<std::uint64_t>(kBackdropBlurIterations),
                                       2u * static_cast<std::uint64_t>(kBackdropBlurIterations));
    VulkanCommandState blurState{};
    for (int i = 0; i < kBackdropBlurIterations; ++i) {
      transition(commandBuffer, backdropScratchTexture_, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL);
      beginColorRendering(commandBuffer,
                          backdropScratchTexture_.view,
                          blurExtent,
                          clear,
                          VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                          renderArea);
      drawBackdropBlurPass(commandBuffer, blurState, blurSource, run.horizontalQuad, run.clip, blurExtent);
      vkCmdEndRendering(commandBuffer);
      transition(commandBuffer, backdropScratchTexture_, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
      ensureTextureDescriptor(backdropScratchTexture_);

      transition(commandBuffer, *blurDestination, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL);
      beginColorRendering(commandBuffer,
                          blurDestination->view,
                          blurExtent,
                          clear,
                          VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                          renderArea);
      drawBackdropBlurPass(commandBuffer, blurState, &backdropScratchTexture_, run.verticalQuad, run.clip, blurExtent);
      vkCmdEndRendering(commandBuffer);
      transition(commandBuffer, *blurDestination, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
      ensureTextureDescriptor(*blurDestination);
      blurSource = blurDestination;
    }
    cacheEntry.signature = signature;
    cacheEntry.valid = true;
    return blurDestination;
  }

  void beginTargetRendering(VkCommandBuffer commandBuffer,
                            VkImage targetImage,
                            VkImageView targetView,
                            VkImageLayout &targetLayout,
                            VkClearValue const &clear,
                            VkAttachmentLoadOp loadOp) {
    transition(commandBuffer, targetImage, targetLayout, VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL);
    targetLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    beginColorRendering(commandBuffer,
                        targetView,
                        swapExtent_,
                        clear,
                        loadOp,
                        currentFramebufferPixelRect());
  }

  void drawOpsWithStackedBackdropBlur(VkCommandBuffer commandBuffer,
                                      VkImage targetImage,
                                      VkImageView targetView,
                                      VkImageLayout &targetLayout,
                                      VkClearValue const &clear,
                                      VkAttachmentLoadOp initialLoadOp,
                                      std::vector<BackdropBlurRun> const &runs) {
    auto const start = std::chrono::steady_clock::now();
    std::uint64_t replayedOps = 0;
    beginTargetRendering(commandBuffer, targetImage, targetView, targetLayout, clear, initialLoadOp);
    std::size_t cursor = 0;
    for (std::size_t runIndex = 0; runIndex < runs.size(); ++runIndex) {
      BackdropBlurRun const &run = runs[runIndex];
      if (run.start > cursor) {
        drawOps(commandBuffer, cursor, run.start);
        replayedOps += run.start - cursor;
      }

      CachedBackdropBlur &cacheEntry = ensureBackdropBlurCacheEntry(runIndex);
      std::uint64_t const signature = backdropBlurSignature(run, runIndex);
      bool const cacheHit = cacheEntry.valid && cacheEntry.signature == signature;
      debug::perf::recordBackdropBlurCacheLookup(cacheHit);
      Texture *blurTexture = cacheHit ? &cacheEntry.texture : nullptr;
      if (!blurTexture) {
        vkCmdEndRendering(commandBuffer);
        copyTargetToBackdropScene(commandBuffer, targetImage, targetLayout, run.clip);
        blurTexture = blurBackdropScene(commandBuffer, run, clear, cacheEntry, signature);
        beginTargetRendering(commandBuffer, targetImage, targetView, targetLayout, clear, VK_ATTACHMENT_LOAD_OP_LOAD);
      }
      drawOps(commandBuffer, run.start, run.end, blurTexture);
      replayedOps += run.end - run.start;
      cursor = run.end;
    }

    if (cursor < ops_.size()) {
      drawOps(commandBuffer, cursor, ops_.size());
      replayedOps += ops_.size() - cursor;
    }
    vkCmdEndRendering(commandBuffer);
    debug::perf::recordVulkanStackedBlur(std::chrono::steady_clock::now() - start, replayedOps);
  }

  bool hasBackdropBlurOps() const {
    return std::any_of(ops_.begin(), ops_.end(), [](DrawOp const &op) {
      return op.kind == DrawOp::Kind::BackdropBlur;
    });
  }

  std::size_t firstBackdropBlurOp() const {
    auto const it = std::find_if(ops_.begin(), ops_.end(), [](DrawOp const &op) {
      return op.kind == DrawOp::Kind::BackdropBlur;
    });
    return it == ops_.end() ? ops_.size() : static_cast<std::size_t>(std::distance(ops_.begin(), it));
  }

  void drawOps(VkCommandBuffer commandBuffer, std::size_t start = 0,
               std::size_t end = std::numeric_limits<std::size_t>::max(),
               Texture *backdropSource = nullptr,
               VkExtent2D renderExtent = {}) {
    auto const recordStart = std::chrono::steady_clock::now();
    std::size_t const opEnd = std::min(end, ops_.size());
    if (renderExtent.width == 0 || renderExtent.height == 0) {
      renderExtent = currentFramebufferExtent();
    }
    bool hasScissor = false;
    Rect currentScissor{};
    VulkanCommandState commandState{};
    VulkanDrawCommandContext const drawContext = drawCommandContext();
    std::uint64_t visited = 0;
    std::uint64_t submitted = 0;
    std::uint64_t scissorChanges = 0;
    for (std::size_t index = std::min(start, opEnd); index < opEnd; ++index) {
      DrawOp const &op = ops_[index];
      ++visited;
      if (op.clip.width <= 0.f || op.clip.height <= 0.f) {
        continue;
      }
      if (!hasScissor || !sameRect(currentScissor, op.clip)) {
        setViewportScissor(commandBuffer, op.clip, renderExtent);
        currentScissor = op.clip;
        hasScissor = true;
        ++scissorChanges;
      }
      ++submitted;
      switch (op.kind) {
      case DrawOp::Kind::Rect:
        drawVulkanRectRange(commandBuffer, commandState, drawContext, op.first, op.count,
                            op.externalStorageDescriptor, op.externalTranslationX,
                            op.externalTranslationY);
        break;
      case DrawOp::Kind::Callout:
        drawVulkanCalloutRange(commandBuffer, commandState, drawContext, op.first, op.count,
                               op.externalTranslationX, op.externalTranslationY);
        break;
      case DrawOp::Kind::Path:
        drawVulkanPathRange(commandBuffer, commandState, drawContext, op.first, op.count,
                            op.externalVertexBuffer, op.externalTranslationX,
                            op.externalTranslationY);
        break;
      case DrawOp::Kind::Image:
        drawVulkanImageRange(commandBuffer, commandState, drawContext, op.texture, op.first,
                             op.count, op.externalStorageDescriptor, op.externalTranslationX,
                             op.externalTranslationY, op.premultipliedAlpha);
        break;
      case DrawOp::Kind::BackdropBlur:
        drawVulkanBackdropRange(commandBuffer, commandState, drawContext, backdropSource,
                                op.first, op.count, op.externalStorageDescriptor,
                                op.externalTranslationX, op.externalTranslationY);
        break;
      }
    }
    debug::perf::recordVulkanDrawOps(std::chrono::steady_clock::now() - recordStart,
                                     visited,
                                     submitted,
                                     scissorChanges);
  }

  void beginColorRendering(VkCommandBuffer commandBuffer, VkImageView view, VkExtent2D extent,
                           VkClearValue const &clear, VkAttachmentLoadOp loadOp) {
    beginColorRendering(commandBuffer,
                        view,
                        extent,
                        clear,
                        loadOp,
                        PixelRect{0u, 0u, extent.width, extent.height});
  }

  void beginColorRendering(VkCommandBuffer commandBuffer,
                           VkImageView view,
                           VkExtent2D extent,
                           VkClearValue const &clear,
                           VkAttachmentLoadOp loadOp,
                           PixelRect renderArea) {
    auto color = vkStructure<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    color.imageView = view;
    color.imageLayout = VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL;
    color.loadOp = loadOp;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue = clear;
    auto rendering = vkStructure<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    std::uint32_t const x0 = std::min(renderArea.x, extent.width);
    std::uint32_t const y0 = std::min(renderArea.y, extent.height);
    std::uint32_t const x1 = std::min(renderArea.x + renderArea.width, extent.width);
    std::uint32_t const y1 = std::min(renderArea.y + renderArea.height, extent.height);
    rendering.renderArea.offset = {static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0)};
    rendering.renderArea.extent = {x1 > x0 ? x1 - x0 : 0u, y1 > y0 ? y1 - y0 : 0u};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(commandBuffer, &rendering);
  }

  void setViewportScissor(VkCommandBuffer commandBuffer, Rect clip) {
    setViewportScissor(commandBuffer, clip, currentFramebufferExtent());
  }

  void setViewportScissor(VkCommandBuffer commandBuffer, Rect clip, VkExtent2D extent) {
    float const renderWidth = static_cast<float>(std::max(1u, extent.width));
    float const renderHeight = static_cast<float>(std::max(1u, extent.height));
    VkViewport vp{0.f, 0.f, renderWidth, renderHeight, 0.f, 1.f};
    vkCmdSetViewport(commandBuffer, 0, 1, &vp);

    float const scaleX = renderWidth / std::max(1.f, static_cast<float>(width_));
    float const scaleY = renderHeight / std::max(1.f, static_cast<float>(height_));
    float const maxX = renderWidth;
    float const maxY = renderHeight;
    float const x0f = std::clamp(std::floor(clip.x * scaleX), 0.f, maxX);
    float const y0f = std::clamp(std::floor(clip.y * scaleY), 0.f, maxY);
    float const x1f = std::clamp(std::ceil((clip.x + clip.width) * scaleX), 0.f, maxX);
    float const y1f = std::clamp(std::ceil((clip.y + clip.height) * scaleY), 0.f, maxY);
    std::uint32_t const x0 = static_cast<std::uint32_t>(x0f);
    std::uint32_t const y0 = static_cast<std::uint32_t>(y0f);
    std::uint32_t const x1 = static_cast<std::uint32_t>(x1f);
    std::uint32_t const y1 = static_cast<std::uint32_t>(y1f);
    VkRect2D sc{{static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0)},
                {x1 > x0 ? x1 - x0 : 0u, y1 > y0 ? y1 - y0 : 0u}};
    vkCmdSetScissor(commandBuffer, 0, 1, &sc);
  }
  Texture *ensureImageTexture(VulkanImage const &image) {
    auto it = imageTextures_.find(&image);
    if (it != imageTextures_.end())
      return it->second.get();
    auto tex = std::make_unique<Texture>();
    bool importedNeedsTransition = false;
    try {
      if (image.external || image.ownsGpuResource) {
        tex->image = image.image;
        tex->view = image.view;
        tex->format = image.format;
        tex->layout = image.ownsImportedMemory ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL;
        tex->width = image.width;
        tex->height = image.height;
        tex->contentGeneration = image.contentGeneration;
        tex->ownsImage = false;
        tex->ownsView = false;
        importedNeedsTransition = image.ownsImportedMemory;
      } else {
        createTexture(*tex, image.width, image.height, image.format);
        tex->contentGeneration = image.contentGeneration;
        queueTextureUpload(*tex, image.pixels.data());
      }
      ensureTextureDescriptor(*tex);
    } catch (...) {
      destroyTexture(*tex);
      throw;
    }
    auto [inserted, ok] = imageTextures_.emplace(&image, std::move(tex));
    (void)ok;
    if (importedNeedsTransition) {
      queueTextureTransition(*inserted->second, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
    }
    return inserted->second.get();
  }

  void destroyDeferredTextures(bool force) {
    for (auto it = pendingTextureDestroys_.begin(); it != pendingTextureDestroys_.end();) {
      if (!force && it->framesRemaining > 0) {
        --it->framesRemaining;
        ++it->successfulSubmits;
      }
      if (force || it->framesRemaining == 0) {
        assert(force || it->successfulSubmits >= kMaxFramesInFlight);
        if (it->texture) {
          destroyTexture(*it->texture);
        }
        it = pendingTextureDestroys_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void destroyDeferredBuffers(bool force) {
    for (auto it = pendingBufferDestroys_.begin(); it != pendingBufferDestroys_.end();) {
      if (!force && it->framesRemaining > 0) {
        --it->framesRemaining;
        ++it->successfulSubmits;
      }
      if (force || it->framesRemaining == 0) {
        assert(force || it->successfulSubmits >= kMaxFramesInFlight);
        if (!force && it->recycleUploadStaging) {
          recycleUploadStagingBuffer(takeBuffer(it->buffer));
        } else {
          destroyBuffer(it->buffer);
        }
        it = pendingBufferDestroys_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void destroyDeferredRecorderResources(bool force) {
    for (auto it = pendingRecorderResourceDestroys_.begin();
         it != pendingRecorderResourceDestroys_.end();) {
      if (!force && it->framesRemaining > 0) {
        --it->framesRemaining;
        ++it->successfulSubmits;
      }
      if (force || it->framesRemaining == 0) {
        assert(force || it->successfulSubmits >= kMaxFramesInFlight);
        destroyVulkanFrameRecorderResourcesNow(it->resources);
        it = pendingRecorderResourceDestroys_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void destroyDeferredOwnedImages(bool force) {
    for (auto it = pendingOwnedImageDestroys_.begin(); it != pendingOwnedImageDestroys_.end();) {
      if (!force && it->framesRemaining > 0) {
        --it->framesRemaining;
        ++it->successfulSubmits;
      }
      if (force || it->framesRemaining == 0) {
        assert(force || it->successfulSubmits >= kMaxFramesInFlight);
        destroyOwnedVulkanImage(*it);
        it = pendingOwnedImageDestroys_.erase(it);
      } else {
        ++it;
      }
    }
  }

  void recycleUploadStagingBuffer(Buffer buffer) {
    if (!buffer.buffer) {
      return;
    }
    constexpr std::size_t kMaxUploadStagingPoolBuffers = 32;
    if (uploadStagingPool_.size() >= kMaxUploadStagingPoolBuffers) {
      destroyBuffer(buffer);
      return;
    }
    uploadStagingPool_.push_back(buffer);
  }

  Buffer takeUploadStagingBuffer(VkDeviceSize size) {
    for (auto it = uploadStagingPool_.begin(); it != uploadStagingPool_.end(); ++it) {
      if (it->buffer && it->capacity >= size) {
        Buffer buffer = *it;
        uploadStagingPool_.erase(it);
        return buffer;
      }
    }
    Buffer buffer{};
    ensureBuffer(buffer, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    return buffer;
  }

  void createTexture(Texture& tex,
                     int width,
                     int height,
                     VkFormat format) {
    tex.width = width;
    tex.height = height;
    tex.format = format == VK_FORMAT_UNDEFINED ? VK_FORMAT_R8G8B8A8_UNORM : format;
    auto image = vkStructure<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = tex.format;
    image.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vkCheck(vmaCreateImage(allocator_, &image, &allocation, &tex.image, &tex.allocation, nullptr),
            "vmaCreateImage");
    auto view = vkStructure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    view.image = tex.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = tex.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    vkCheck(vkCreateImageView(device_, &view, nullptr, &tex.view), "vkCreateImageView");
  }

  void queueTextureUpload(Texture& tex, void const* pixels) {
    if (!pixels || tex.width <= 0 || tex.height <= 0)
      return;
    VkDeviceSize size = static_cast<VkDeviceSize>(tex.width) * tex.height * 4u;
    for (auto it = pendingTextureUploads_.begin(); it != pendingTextureUploads_.end();) {
      if (it->texture == &tex) {
        recycleUploadStagingBuffer(takeBuffer(it->staging));
        it = pendingTextureUploads_.erase(it);
      } else {
        ++it;
      }
    }
    PendingTextureUpload uploadJob{};
    uploadJob.texture = &tex;
    uploadJob.width = static_cast<std::uint32_t>(tex.width);
    uploadJob.height = static_cast<std::uint32_t>(tex.height);
    uploadJob.staging = takeUploadStagingBuffer(size);
    upload(uploadJob.staging, pixels, static_cast<std::size_t>(size));
    pendingTextureUploads_.push_back(std::move(uploadJob));
  }

  void queueTextureRegionUpload(Texture& tex,
                                std::uint32_t x,
                                std::uint32_t y,
                                std::uint32_t width,
                                std::uint32_t height,
                                void const* pixels,
                                std::uint32_t sourceBytesPerRow = 0) {
    if (!pixels || tex.width <= 0 || tex.height <= 0 || width == 0 || height == 0)
      return;
    if (x > static_cast<std::uint32_t>(tex.width) || y > static_cast<std::uint32_t>(tex.height) ||
        width > static_cast<std::uint32_t>(tex.width) - x ||
        height > static_cast<std::uint32_t>(tex.height) - y)
      return;
    for (auto const& uploadJob : pendingTextureUploads_) {
      if (uploadJob.texture == &tex && uploadJob.x == 0 && uploadJob.y == 0 &&
          uploadJob.width == static_cast<std::uint32_t>(tex.width) &&
          uploadJob.height == static_cast<std::uint32_t>(tex.height)) {
        return;
      }
    }
    PendingTextureUpload uploadJob{};
    uploadJob.texture = &tex;
    uploadJob.x = x;
    uploadJob.y = y;
    uploadJob.width = width;
    uploadJob.height = height;
    VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4u;
    std::size_t const rowBytes = static_cast<std::size_t>(width) * 4u;
    std::size_t const stride = sourceBytesPerRow == 0 ? rowBytes : sourceBytesPerRow;
    if (stride < rowBytes) {
      return;
    }
    uploadJob.staging = takeUploadStagingBuffer(size);
    uploadRows(uploadJob.staging, pixels, rowBytes, height, stride);
    pendingTextureUploads_.push_back(std::move(uploadJob));
  }

  void queueTextureTransition(Texture& tex, VkImageLayout layout) {
    if (!tex.image || tex.layout == layout)
      return;
    for (PendingTextureTransition& transition : pendingTextureTransitions_) {
      if (transition.texture == &tex) {
        transition.layout = layout;
        return;
      }
    }
    pendingTextureTransitions_.push_back(PendingTextureTransition{&tex, layout});
  }

  void recordTextureUpload(VkCommandBuffer cmd, Texture& tex, PendingTextureUpload& uploadJob) {
    Buffer& staging = uploadJob.staging;
    if (!staging.buffer || tex.width <= 0 || tex.height <= 0)
      return;
    std::uint32_t const copyWidth = uploadJob.width > 0 ? uploadJob.width : static_cast<std::uint32_t>(tex.width);
    std::uint32_t const copyHeight = uploadJob.height > 0 ? uploadJob.height : static_cast<std::uint32_t>(tex.height);
    if (uploadJob.x > static_cast<std::uint32_t>(tex.width) ||
        uploadJob.y > static_cast<std::uint32_t>(tex.height) ||
        copyWidth > static_cast<std::uint32_t>(tex.width) - uploadJob.x ||
        copyHeight > static_cast<std::uint32_t>(tex.height) - uploadJob.y) {
      return;
    }
    transition(cmd, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageOffset = {static_cast<std::int32_t>(uploadJob.x), static_cast<std::int32_t>(uploadJob.y), 0};
    copy.imageExtent = {copyWidth, copyHeight, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    transition(cmd, tex, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL);
  }

  void recordPendingTextureUploads(VkCommandBuffer cmd) {
    for (auto& uploadJob : pendingTextureUploads_) {
      if (uploadJob.texture) {
        recordTextureUpload(cmd, *uploadJob.texture, uploadJob);
      }
      pendingBufferDestroys_.push_back(PendingBufferDestroy{takeBuffer(uploadJob.staging),
                                                            kMaxFramesInFlight + 1u,
                                                            0u,
                                                            true});
    }
    pendingTextureUploads_.clear();
  }

  void destroyPendingTextureUploads() {
    for (PendingTextureUpload& uploadJob : pendingTextureUploads_) {
      destroyBuffer(uploadJob.staging);
    }
    pendingTextureUploads_.clear();
  }

  void recordPendingTextureTransitions(VkCommandBuffer cmd) {
    for (PendingTextureTransition const& pending : pendingTextureTransitions_) {
      if (pending.texture) {
        transition(cmd, *pending.texture, pending.layout);
      }
    }
    pendingTextureTransitions_.clear();
  }

  void retireDeferredResourcesAfterSubmit() {
    destroyDeferredTextures(false);
    destroyDeferredBuffers(false);
    destroyDeferredRecorderResources(false);
    destroyDeferredOwnedImages(false);
    retireSwapchains(false);
  }

  bool swapchainImageStateReadyForAcquire() const {
    if (swapchainImages_.empty()) {
      return false;
    }
    if (imageInFlightFences_.size() != swapchainImages_.size() ||
        imageRenderFinished_.size() != swapchainImages_.size()) {
      return false;
    }
    return std::all_of(imageRenderFinished_.begin(), imageRenderFinished_.end(), [](VkSemaphore semaphore) {
      return semaphore != VK_NULL_HANDLE;
    });
  }

  VkFormat backdropRenderTargetFormat() const {
    VkFormat constexpr preferred = VK_FORMAT_R16G16B16A16_SFLOAT;
    VkFormatProperties properties{};
    vkGetPhysicalDeviceFormatProperties(physical_, preferred, &properties);
    VkFormatFeatureFlags constexpr required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                              VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                              VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((properties.optimalTilingFeatures & required) == required) return preferred;
    return surfaceFormat_.format == VK_FORMAT_UNDEFINED ? VK_FORMAT_B8G8R8A8_UNORM : surfaceFormat_.format;
  }

  void createRenderTargetTexture(Texture &tex,
                                 int width,
                                 int height,
                                 VkFormat requestedFormat = VK_FORMAT_UNDEFINED) {
    tex.width = width;
    tex.height = height;
    tex.format = requestedFormat == VK_FORMAT_UNDEFINED
                     ? (surfaceFormat_.format == VK_FORMAT_UNDEFINED ? VK_FORMAT_B8G8R8A8_UNORM : surfaceFormat_.format)
                     : requestedFormat;
    auto image = vkStructure<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = tex.format;
    image.extent = {static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), 1};
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                  VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                  VK_IMAGE_USAGE_SAMPLED_BIT;
    image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    vkCheck(vmaCreateImage(allocator_, &image, &allocation, &tex.image, &tex.allocation, nullptr),
            "vmaCreateImage");
    auto view = vkStructure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    view.image = tex.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = image.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    vkCheck(vkCreateImageView(device_, &view, nullptr, &tex.view), "vkCreateImageView");
    tex.layout = VK_IMAGE_LAYOUT_UNDEFINED;
  }

  void ensureTextureDescriptor(Texture &tex) {
    if (tex.descriptor)
      return;
    auto alloc = vkStructure<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    alloc.descriptorPool = resources().descriptorPool;
    alloc.descriptorSetCount = 1;
    VkDescriptorSetLayout const layout = resources().textureDescriptorLayout;
    alloc.pSetLayouts = &layout;
    vkCheck(vkAllocateDescriptorSets(device_, &alloc, &tex.descriptor), "vkAllocateDescriptorSets");
    VkDescriptorImageInfo ii{resources().sampler, tex.view, VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL};
    auto write = vkStructure<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
    write.dstSet = tex.descriptor;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }

  void destroyTexture(Texture &tex) {
    if (tex.descriptor && resources().descriptorPool)
      vkFreeDescriptorSets(device_, resources().descriptorPool, 1, &tex.descriptor);
    if (tex.view && tex.ownsView)
      vkDestroyImageView(device_, tex.view, nullptr);
    if (tex.image && tex.ownsImage)
      vmaDestroyImage(allocator_, tex.image, tex.allocation);
    tex = {};
  }

  static VkPipelineStageFlags2 stageMaskForLayout(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
      return VK_PIPELINE_STAGE_2_NONE;
    case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:
      return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      return VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL:
      return VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR:
      return VK_PIPELINE_STAGE_2_NONE;
    default:
      return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
  }

  static VkAccessFlags2 accessMaskForLayout(VkImageLayout layout, bool source) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL:
      return source ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                    : VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      return VK_ACCESS_2_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      return VK_ACCESS_2_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL:
      return VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    default:
      return VK_ACCESS_2_NONE;
    }
  }

  void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) {
    if (oldLayout == newLayout)
      return;
    auto barrier = vkStructure<VkImageMemoryBarrier2>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2);
    barrier.srcStageMask = stageMaskForLayout(oldLayout);
    barrier.srcAccessMask = accessMaskForLayout(oldLayout, true);
    barrier.dstStageMask = stageMaskForLayout(newLayout);
    barrier.dstAccessMask = accessMaskForLayout(newLayout, false);
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    auto dependency = vkStructure<VkDependencyInfo>(VK_STRUCTURE_TYPE_DEPENDENCY_INFO);
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
  }

  void transition(VkCommandBuffer cmd, Texture &texture, VkImageLayout newLayout) {
    transition(cmd, texture.image, texture.layout, newLayout);
    texture.layout = newLayout;
  }

  bool frameCaptureTraceEnabled() const {
    char const* raw = std::getenv("LAMBDA_VULKAN_FRAME_CAPTURE_TRACE");
    return raw && *raw && std::strcmp(raw, "0") != 0 && std::strcmp(raw, "false") != 0 &&
           std::strcmp(raw, "FALSE") != 0;
  }

  bool readbackFenceReady(VkFence fence, bool wait, char const* label) {
    if (!fence) {
      return true;
    }
    if (wait) {
      vkCheck(vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX), label);
      return true;
    }
    VkResult const status = vkGetFenceStatus(device_, fence);
    if (status == VK_SUCCESS) {
      return true;
    }
    if (status == VK_NOT_READY) {
      return false;
    }
    vkCheck(status, label);
    return false;
  }

  void writeDebugScreenshotIfRequested(VkCommandBuffer commandBuffer, VkImage source, VkFence frameFence) {
    if (debugScreenshotWritten_)
      return;
    static char const *const path = std::getenv("LAMBDA_DEBUG_SCREENSHOT_PATH");
    if (!path || !*path)
      return;
    Buffer staging{};
    VkDeviceSize size = static_cast<VkDeviceSize>(framebufferWidth_) * framebufferHeight_ * 4u;
    ensureBuffer(staging, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {static_cast<std::uint32_t>(framebufferWidth_),
                        static_cast<std::uint32_t>(framebufferHeight_), 1};
    vkCmdCopyImageToBuffer(commandBuffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &copy);
    pendingScreenshotPath_ = path;
    pendingScreenshotBuffer_ = staging;
    pendingScreenshotSize_ = size;
    pendingScreenshotFence_ = frameFence;
    debugScreenshotWritten_ = true;
  }

  void captureFrameIfRequested(VkCommandBuffer commandBuffer,
                               VkImage source,
                               VkFence frameFence,
                               bool awaitingExternalSubmit = false) {
    if (!frameCaptureRequested_) {
      return;
    }
    frameCaptureRequested_ = false;
    if (pendingFrameCaptureBuffer_.buffer) {
      destroyBuffer(pendingFrameCaptureBuffer_);
      pendingFrameCaptureFence_ = VK_NULL_HANDLE;
      pendingFrameCaptureExternal_ = false;
      pendingFrameCaptureAwaitingExternalSubmit_ = false;
      pendingFrameCaptureTracePolls_ = 0;
    }
    Buffer staging{};
    VkDeviceSize size = static_cast<VkDeviceSize>(framebufferWidth_) * framebufferHeight_ * 4u;
    ensureBuffer(staging, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {static_cast<std::uint32_t>(framebufferWidth_),
                        static_cast<std::uint32_t>(framebufferHeight_), 1};
    vkCmdCopyImageToBuffer(commandBuffer, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer, 1, &copy);
    pendingFrameCaptureBuffer_ = staging;
    pendingFrameCaptureSize_ = size;
    pendingFrameCaptureWidth_ = static_cast<std::uint32_t>(framebufferWidth_);
    pendingFrameCaptureHeight_ = static_cast<std::uint32_t>(framebufferHeight_);
    pendingFrameCaptureBgra_ = surfaceFormat_.format == VK_FORMAT_B8G8R8A8_UNORM ||
                               surfaceFormat_.format == VK_FORMAT_B8G8R8A8_SRGB;
    pendingFrameCaptureFence_ = frameFence;
    pendingFrameCaptureExternal_ = awaitingExternalSubmit;
    pendingFrameCaptureAwaitingExternalSubmit_ = awaitingExternalSubmit;
    pendingFrameCaptureTracePolls_ = 0;
    if (frameCaptureTraceEnabled()) {
      std::fprintf(stderr,
                   "Lambda Vulkan: frame capture queued external=%d awaitingSubmit=%d fence=%d size=%ux%u\n",
                   awaitingExternalSubmit ? 1 : 0,
                   pendingFrameCaptureAwaitingExternalSubmit_ ? 1 : 0,
                   frameFence ? 1 : 0,
                   pendingFrameCaptureWidth_,
                   pendingFrameCaptureHeight_);
    }
  }

  bool pollFrameCapture(bool wait) {
    if (!pendingFrameCaptureBuffer_.buffer || pendingFrameCaptureWidth_ == 0 || pendingFrameCaptureHeight_ == 0) {
      return false;
    }
    ++pendingFrameCaptureTracePolls_;
    if (pendingFrameCaptureAwaitingExternalSubmit_) {
      if (frameCaptureTraceEnabled() && pendingFrameCaptureTracePolls_ <= 8) {
        std::fprintf(stderr, "Lambda Vulkan: frame capture poll awaiting external submit poll=%d\n",
                     pendingFrameCaptureTracePolls_);
      }
      return false;
    }
    if (pendingFrameCaptureExternal_ && pendingFrameCaptureFence_ == VK_NULL_HANDLE) {
      if (frameCaptureTraceEnabled()) {
        std::fprintf(stderr, "Lambda Vulkan: frame capture external readback has no fence; waiting queue idle\n");
      }
      vkCheck(vkQueueWaitIdle(queue_), "vkQueueWaitIdle(frameCapture)");
    }
    if (!readbackFenceReady(pendingFrameCaptureFence_, wait, "vkGetFenceStatus(frameCapture)")) {
      if (frameCaptureTraceEnabled() && pendingFrameCaptureTracePolls_ <= 8) {
        std::fprintf(stderr, "Lambda Vulkan: frame capture poll fence not ready poll=%d wait=%d\n",
                     pendingFrameCaptureTracePolls_,
                     wait ? 1 : 0);
      }
      return false;
    }
    if (frameCaptureTraceEnabled()) {
      std::fprintf(stderr, "Lambda Vulkan: frame capture readback ready poll=%d\n",
                   pendingFrameCaptureTracePolls_);
    }
    void* mapped = nullptr;
    vkCheck(vmaInvalidateAllocation(allocator_,
                                    pendingFrameCaptureBuffer_.allocation,
                                    0,
                                    pendingFrameCaptureSize_),
            "vmaInvalidateAllocation");
    vkCheck(vmaMapMemory(allocator_, pendingFrameCaptureBuffer_.allocation, &mapped), "vmaMapMemory");
    auto const* source = static_cast<std::uint8_t const*>(mapped);
    capturedFrameBytes_.resize(static_cast<std::size_t>(pendingFrameCaptureWidth_) *
                               pendingFrameCaptureHeight_ * 4u);
    if (pendingFrameCaptureBgra_) {
      std::memcpy(capturedFrameBytes_.data(), source, capturedFrameBytes_.size());
    } else {
      for (std::size_t i = 0; i < capturedFrameBytes_.size(); i += 4u) {
        capturedFrameBytes_[i + 0u] = source[i + 2u];
        capturedFrameBytes_[i + 1u] = source[i + 1u];
        capturedFrameBytes_[i + 2u] = source[i + 0u];
        capturedFrameBytes_[i + 3u] = source[i + 3u];
      }
    }
    vmaUnmapMemory(allocator_, pendingFrameCaptureBuffer_.allocation);
    destroyBuffer(pendingFrameCaptureBuffer_);
    capturedFrameWidth_ = pendingFrameCaptureWidth_;
    capturedFrameHeight_ = pendingFrameCaptureHeight_;
    capturedFrameAvailable_ = true;
    pendingFrameCaptureSize_ = 0;
    pendingFrameCaptureWidth_ = 0;
    pendingFrameCaptureHeight_ = 0;
    pendingFrameCaptureBgra_ = true;
    pendingFrameCaptureFence_ = VK_NULL_HANDLE;
    pendingFrameCaptureExternal_ = false;
    pendingFrameCaptureAwaitingExternalSubmit_ = false;
    pendingFrameCaptureTracePolls_ = 0;
    return true;
  }

  bool pollScreenshot(bool wait) {
    if (!pendingScreenshotBuffer_.buffer || pendingScreenshotPath_.empty())
      return false;
    if (!readbackFenceReady(pendingScreenshotFence_, wait, "vkGetFenceStatus(debugScreenshot)")) {
      return false;
    }
    void *mapped = nullptr;
    vkCheck(vmaInvalidateAllocation(allocator_, pendingScreenshotBuffer_.allocation, 0, pendingScreenshotSize_),
            "vmaInvalidateAllocation");
    vkCheck(vmaMapMemory(allocator_, pendingScreenshotBuffer_.allocation, &mapped), "vmaMapMemory");
    std::ofstream out(pendingScreenshotPath_, std::ios::binary);
    out << "P6\n"
        << framebufferWidth_ << " " << framebufferHeight_ << "\n255\n";
    auto *bytes = static_cast<std::uint8_t const *>(mapped);
    bool const bgra = surfaceFormat_.format == VK_FORMAT_B8G8R8A8_UNORM ||
                      surfaceFormat_.format == VK_FORMAT_B8G8R8A8_SRGB;
    for (int i = 0; i < framebufferWidth_ * framebufferHeight_; ++i) {
      out.put(static_cast<char>(bytes[i * 4 + (bgra ? 2 : 0)]));
      out.put(static_cast<char>(bytes[i * 4 + 1]));
      out.put(static_cast<char>(bytes[i * 4 + (bgra ? 0 : 2)]));
    }
    vmaUnmapMemory(allocator_, pendingScreenshotBuffer_.allocation);
    destroyBuffer(pendingScreenshotBuffer_);
    pendingScreenshotPath_.clear();
    pendingScreenshotFence_ = VK_NULL_HANDLE;
    return true;
  }

  void pollReadbacks(bool wait) {
    pollFrameCapture(wait);
    pollScreenshot(wait);
  }
  unsigned int handle_ = 0;
  TextSystem &textSystem_;
  VulkanGlyphAtlas glyphAtlas_;
  VulkanPipelines pipelines_;
  VulkanSwapchain swapchainController_;
  VulkanRenderTargetSpec targetSpec_{};
  int width_ = 1;
  int height_ = 1;
  int framebufferWidth_ = 1;
  int framebufferHeight_ = 1;
  int swapchainTargetWidth_ = 1;
  int swapchainTargetHeight_ = 1;
  int resizeBoundsHintWidth_ = 0;
  int resizeBoundsHintHeight_ = 0;
  float dpiScaleX_ = 1.f;
  float dpiScaleY_ = 1.f;
  Color clearColor_ = Colors::transparent;
  DrawState state_{};
  std::vector<DrawState> stateStack_;
  VulkanFrameRecorder *captureTarget_ = nullptr;
  DrawState captureSavedState_{};
  std::vector<DrawState> captureSavedStack_;
  bool hasCaptureSavedState_ = false;
  std::vector<RectInstance> rects_;
  std::vector<CalloutInstance> callouts_;
  std::vector<QuadInstance> quads_;
  std::vector<ImageBatch> batches_;
  std::vector<DrawOp> ops_;
  std::vector<VulkanPathVertex> pathVerts_;
  std::unordered_map<PathCacheKey, CachedPath, PathCacheKeyHash> pathCache_;
  std::list<PathCacheKey> pathCacheLru_;
  std::size_t cachedPathVertexCount_ = 0;

  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VmaAllocator allocator_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  SharedVulkanCore *shared_ = nullptr;
  PFN_vkGetPastPresentationTimingGOOGLE getPastPresentationTiming_ = nullptr;
  std::uint32_t nextPresentId_ = 1;
  std::uint32_t lastSubmittedPresentId_ = 0;
  std::uint32_t queueFamily_ = 0;
  VkCommandPool commandPool_ = VK_NULL_HANDLE;
  std::array<VkCommandBuffer, kMaxFramesInFlight> commandBuffers_{};
  std::array<VkSemaphore, kMaxFramesInFlight> imageAvailable_{};
  std::array<VkFence, kMaxFramesInFlight> frameFences_{};
  std::array<std::uint64_t, kMaxFramesInFlight> frameFenceSubmitGenerations_{};
  std::array<std::uint64_t, kMaxFramesInFlight> frameFenceCompleteGenerations_{};
  static constexpr std::size_t kNoResetFrameFence = static_cast<std::size_t>(-1);
  std::size_t resetFrameFenceIndex_ = kNoResetFrameFence;
  std::size_t currentFrame_ = 0;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;
  VkSurfaceFormatKHR surfaceFormat_{};
  VkExtent2D swapExtent_{};
  std::vector<VkImage> swapchainImages_;
  std::vector<VkFence> imageInFlightFences_;
  std::vector<VkFence> imagePresentFences_;
  std::vector<VkImageView> swapchainViews_;
  std::vector<VkSemaphore> imageRenderFinished_;
  std::vector<RetiredSwapchain> retiredSwapchains_;
  Texture backdropSceneTexture_;
  Texture backdropScratchTexture_;
  Texture backdropBlurTexture_;
  std::vector<CachedBackdropBlur> backdropBlurCache_;
  std::uint32_t backdropBlurBaseDownsample_ = kDefaultBackdropBlurBaseDownsample;
  std::array<FrameGeometryResources, kMaxFramesInFlight> frameGeometry_{};
  Buffer pendingScreenshotBuffer_;
  VkDeviceSize pendingScreenshotSize_ = 0;
  std::string pendingScreenshotPath_;
  VkFence pendingScreenshotFence_ = VK_NULL_HANDLE;
  bool debugScreenshotWritten_ = false;
  Buffer pendingFrameCaptureBuffer_;
  VkDeviceSize pendingFrameCaptureSize_ = 0;
  std::uint32_t pendingFrameCaptureWidth_ = 0;
  std::uint32_t pendingFrameCaptureHeight_ = 0;
  bool pendingFrameCaptureBgra_ = true;
  VkFence pendingFrameCaptureFence_ = VK_NULL_HANDLE;
  bool pendingFrameCaptureExternal_ = false;
  bool pendingFrameCaptureAwaitingExternalSubmit_ = false;
  int pendingFrameCaptureTracePolls_ = 0;
  bool frameCaptureRequested_ = false;
  bool capturedFrameAvailable_ = false;
  std::vector<std::uint8_t> capturedFrameBytes_;
  std::uint32_t capturedFrameWidth_ = 0;
  std::uint32_t capturedFrameHeight_ = 0;
  bool deviceWaitIdleOnDestruct_ = true;
  bool swapchainDirty_ = true;
  bool presentFenceRuntimeDisabled_ = false;
  bool transparentSurface_ = false;
  bool imagePremultipliedAlpha_ = false;
  std::unordered_map<VulkanImage const *, std::unique_ptr<Texture>> imageTextures_;
  std::vector<PendingTextureDestroy> pendingTextureDestroys_;
  std::vector<PendingTextureTransition> pendingTextureTransitions_;
  std::vector<PendingTextureUpload> pendingTextureUploads_;
  std::vector<PendingBufferDestroy> pendingBufferDestroys_;
  std::vector<PendingRecorderResourceDestroy> pendingRecorderResourceDestroys_;
  std::vector<Buffer> uploadStagingPool_;
  std::vector<PendingOwnedVulkanImageDestroy> pendingOwnedImageDestroys_;
  std::uint64_t renderTargetFrameSignature_ = 0;
  bool renderTargetFrameCacheValid_ = false;
  bool ownsSharedVulkanCore_ = false;
  bool targetMode_ = false;
};

} // namespace

namespace {

void evictImageTexturesFor(VulkanImage const *image) {
  if (!image)
    return;
  std::lock_guard lock(gCanvasRegistryMutex);
  for (VulkanCanvas* canvas : gCanvases) {
    if (canvas)
      canvas->evictImageTexture(image);
  }
}

bool deferOwnedVulkanImageDestroy(PendingOwnedVulkanImageDestroy destroy) {
  if (!destroy.device || (!destroy.image && !destroy.view && !destroy.importedMemory)) {
    return false;
  }
  std::lock_guard lock(gCanvasRegistryMutex);
  for (VulkanCanvas* canvas : gCanvases) {
    if (canvas && canvas->deferOwnedImageDestroy(destroy)) {
      return true;
    }
  }
  return false;
}

} // namespace

bool deferVulkanFrameRecorderResourcesDestroy(VulkanFrameRecorderResources resources) noexcept {
  if (!resources.device ||
      (!resources.preparedQuadBuffer && !resources.preparedRectBuffer &&
       !resources.preparedPathVertexBuffer && !resources.preparedQuadDescriptor &&
       !resources.preparedRectDescriptor)) {
    return false;
  }
  std::lock_guard lock(gCanvasRegistryMutex);
  for (VulkanCanvas* canvas : gCanvases) {
    if (canvas && canvas->deferFrameRecorderResourcesDestroy(resources)) {
      return true;
    }
  }
  return false;
}

namespace {

void updateImageTexturesFor(VulkanImage const* image) {
  if (!image)
    return;
  std::lock_guard lock(gCanvasRegistryMutex);
  for (VulkanCanvas* canvas : gCanvases) {
    if (canvas)
      canvas->updateImageTexture(image);
  }
}

void updateImageTextureRegionFor(VulkanImage const* image,
                                 std::uint32_t x,
                                 std::uint32_t y,
                                 std::uint32_t width,
                                 std::uint32_t height,
                                 void const* pixels,
                                 std::uint32_t sourceBytesPerRow) {
  if (!image || !pixels)
    return;
  std::lock_guard lock(gCanvasRegistryMutex);
  for (VulkanCanvas* canvas : gCanvases) {
    if (canvas)
      canvas->updateImageTextureRegion(image, x, y, width, height, pixels, sourceBytesPerRow);
  }
}

void markImageTextureContentsChangedFor(VulkanImage const* image) {
  if (!image)
    return;
  std::lock_guard lock(gCanvasRegistryMutex);
  for (VulkanCanvas* canvas : gCanvases) {
    if (canvas)
      canvas->markImageTextureContentsChanged(image);
  }
}

} // namespace

void evictVulkanImageTextures(VulkanImage const* image) noexcept {
  evictImageTexturesFor(image);
}

void updateVulkanImageTextures(VulkanImage const* image) {
  updateImageTexturesFor(image);
}

void updateVulkanImageTextureRegion(VulkanImage const* image,
                                    std::uint32_t x,
                                    std::uint32_t y,
                                    std::uint32_t width,
                                    std::uint32_t height,
                                    void const* pixels,
                                    std::uint32_t sourceBytesPerRow) {
  updateImageTextureRegionFor(image, x, y, width, height, pixels, sourceBytesPerRow);
}

void markVulkanImageTextureContentsChanged(VulkanImage const* image) {
  markImageTextureContentsChangedFor(image);
}

bool deferVulkanImageResourceDestroy(PendingOwnedVulkanImageDestroy destroy) {
  return deferOwnedVulkanImageDestroy(std::move(destroy));
}

void retainSharedVulkanCoreForVulkanImage() {
  acquireSharedVulkanCore(VK_NULL_HANDLE);
}

void releaseSharedVulkanCoreForVulkanImage() {
  releaseSharedVulkanCore();
}

void configureVulkanCanvasRuntime(std::span<char const* const> requiredInstanceExtensions,
                                  std::filesystem::path cacheDir) {
  std::lock_guard lock(gVulkanCoreMutex);
  if (gVulkanCore.instance) {
    if (allExtensionsConfigured(gRequiredInstanceExtensions, requiredInstanceExtensions)) {
      return;
    }
    throw std::runtime_error("Cannot configure Vulkan instance extensions after instance creation");
  }
  for (char const *extension : requiredInstanceExtensions) {
    appendUniqueExtension(gRequiredInstanceExtensions, extension);
  }
  gPipelineCacheDir = std::move(cacheDir);
}

VkInstance ensureSharedVulkanInstance() {
  return ensureSharedVulkanInstanceImpl();
}

std::unique_ptr<Canvas> createVulkanCanvas(VkSurfaceKHR surface,
                                           unsigned int handle,
                                           TextSystem &textSystem,
                                           VulkanCanvasOptions options) {
  return std::make_unique<VulkanCanvas>(surface, handle, textSystem, options);
}

std::unique_ptr<Canvas> createVulkanRenderTargetCanvas(VulkanRenderTargetSpec const& spec,
                                                       TextSystem& textSystem) {
  return std::make_unique<VulkanCanvas>(spec, textSystem);
}

bool setVulkanRenderTargetSpecForCanvas(Canvas* canvas, VulkanRenderTargetSpec const& spec) {
  auto* vulkan = dynamic_cast<VulkanCanvas*>(canvas);
  return vulkan && vulkan->setRenderTargetSpec(spec);
}

bool markVulkanRenderTargetCanvasSubmitted(Canvas* canvas) {
  auto* vulkan = dynamic_cast<VulkanCanvas*>(canvas);
  if (!vulkan) {
    return false;
  }
  vulkan->markExternalRenderTargetSubmitted();
  return true;
}

void setVulkanCanvasResizeBoundsHint(Canvas* canvas, int logicalWidth, int logicalHeight) {
  if (!canvas) {
    return;
  }
  if (auto* vulkan = dynamic_cast<VulkanCanvas*>(canvas)) {
    vulkan->setResizeBoundsHint(logicalWidth, logicalHeight);
  }
}

bool setVulkanCanvasBackdropBlurBaseDownsample(Canvas* canvas, std::uint32_t downsample) {
  if (auto* vulkan = dynamic_cast<VulkanCanvas*>(canvas)) {
    vulkan->setBackdropBlurBaseDownsample(downsample);
    return true;
  }
  return false;
}

bool setVulkanCanvasImagePremultipliedAlpha(Canvas* canvas, bool enabled) {
  auto* vulkan = dynamic_cast<VulkanCanvas*>(canvas);
  return vulkan ? vulkan->setImagePremultipliedAlpha(enabled) : false;
}

bool setVulkanCanvasTransparentSurface(Canvas* canvas, bool enabled) {
  auto* vulkan = dynamic_cast<VulkanCanvas*>(canvas);
  return vulkan ? vulkan->setTransparentSurface(enabled) : false;
}

bool markVulkanImageContentsChanged(Image* image) {
  auto* vulkanImage = dynamic_cast<VulkanImage*>(image);
  if (!vulkanImage) return false;
  vulkanImage->markContentChanged();
  return true;
}

bool drawVulkanBackdropBlurFrame(Canvas* canvas,
                                 Rect const& frame,
                                 CornerRadius const& frameRadius,
                                 Rect const& cutout,
                                 float radius,
                                 Color tint) {
  auto* vulkan = dynamic_cast<VulkanCanvas*>(canvas);
  return vulkan && vulkan->drawBackdropBlurFrame(frame, frameRadius, cutout, radius, tint);
}

bool drawVulkanCalloutMaterial(Canvas* canvas,
                               Rect const& bounds,
                               Rect const& card,
                               CornerRadius const& corners,
                               Color baseColor,
                               Color tintColor,
                               Color borderColor,
                               float borderWidth,
                               VulkanCalloutPlacement placement,
                               float arrowWidth,
                               float arrowHeight) {
  auto* vulkan = dynamic_cast<VulkanCanvas*>(canvas);
  return vulkan && vulkan->drawCalloutMaterial(bounds,
                                               card,
                                               corners,
                                               baseColor,
                                               tintColor,
                                               borderColor,
                                               borderWidth,
                                               placement,
                                               arrowWidth,
                                               arrowHeight);
}

bool vulkanCanvasSupportsDisplayTiming(Canvas* canvas) {
  auto* vulkan = dynamic_cast<VulkanCanvas*>(canvas);
  return vulkan && vulkan->supportsDisplayTiming();
}

bool vulkanCanvasUsesMailboxPresentMode(Canvas* canvas) {
  auto* vulkan = dynamic_cast<VulkanCanvas*>(canvas);
  return vulkan && vulkan->usesMailboxPresentMode();
}

std::uint32_t lastVulkanCanvasPresentId(Canvas* canvas) {
  auto* vulkan = dynamic_cast<VulkanCanvas*>(canvas);
  return vulkan ? vulkan->lastPresentId() : 0u;
}

std::vector<VulkanPastPresentationTiming> pollVulkanCanvasPastPresentationTimings(Canvas* canvas) {
  auto* vulkan = dynamic_cast<VulkanCanvas*>(canvas);
  return vulkan ? vulkan->pollPastPresentationTimings() : std::vector<VulkanPastPresentationTiming>{};
}

std::shared_ptr<Image> Image::fromExternalVulkan(VkImage image, VkImageView view, VkFormat format,
                                                 std::uint32_t width, std::uint32_t height) {
  if (!image || !view || width == 0 || height == 0) {
    return nullptr;
  }
  return std::make_shared<VulkanImage>(image, view, format, width, height);
}

std::shared_ptr<Image> Image::fromDmabuf(DmabufImageSpec const& spec) {
  if (spec.width == 0 || spec.height == 0 || spec.planes.size() != 1) {
    for (DmabufPlane const& plane : spec.planes) {
      if (plane.fd >= 0) close(plane.fd);
    }
    return nullptr;
  }
  DmabufPlane const plane = spec.planes.front();
  int fd = plane.fd;
  if (fd < 0 || plane.stride == 0) {
    if (fd >= 0) close(fd);
    return nullptr;
  }

  VkFormat const format = vkFormatForDrmFormat(spec.drmFormat);
  if (format == VK_FORMAT_UNDEFINED) {
    close(fd);
    return nullptr;
  }

  VkDevice device = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  bool acquiredCoreReference = false;
  try {
    SharedVulkanCore* core = acquireSharedVulkanCore(VK_NULL_HANDLE);
    acquiredCoreReference = true;
    device = core->device;

    auto externalInfo =
        vkStructure<VkExternalMemoryImageCreateInfo>(VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
    externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;

    VkSubresourceLayout planeLayout{};
    planeLayout.offset = plane.offset;
    planeLayout.rowPitch = plane.stride;

    auto modifierInfo = vkStructure<VkImageDrmFormatModifierExplicitCreateInfoEXT>(
        VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT);
    modifierInfo.pNext = &externalInfo;
    modifierInfo.drmFormatModifier = plane.modifier;
    modifierInfo.drmFormatModifierPlaneCount = 1;
    modifierInfo.pPlaneLayouts = &planeLayout;

    auto imageInfo = vkStructure<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    imageInfo.pNext = &modifierInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {spec.width, spec.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCheck(vkCreateImage(device, &imageInfo, nullptr, &image), "vkCreateImage dmabuf");

    auto getMemoryFdProperties = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
        vkGetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR"));
    if (!getMemoryFdProperties) {
      throw std::runtime_error("vkGetMemoryFdPropertiesKHR is unavailable");
    }
    auto fdProps = vkStructure<VkMemoryFdPropertiesKHR>(VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR);
    vkCheck(getMemoryFdProperties(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdProps),
            "vkGetMemoryFdPropertiesKHR");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, image, &requirements);
    std::uint32_t const memoryTypeBits = requirements.memoryTypeBits & fdProps.memoryTypeBits;

    auto dedicated =
        vkStructure<VkMemoryDedicatedAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO);
    dedicated.image = image;
    auto importInfo = vkStructure<VkImportMemoryFdInfoKHR>(VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
    importInfo.pNext = &dedicated;
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    importInfo.fd = fd;

    auto allocateInfo = vkStructure<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
    allocateInfo.pNext = &importInfo;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = findMemoryType(core->physical, memoryTypeBits, 0);
    vkCheck(vkAllocateMemory(device, &allocateInfo, nullptr, &memory), "vkAllocateMemory dmabuf");
    fd = -1; // Ownership transferred to Vulkan on successful import.

    vkCheck(vkBindImageMemory(device, image, memory, 0), "vkBindImageMemory dmabuf");

    auto viewInfo = vkStructure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    vkCheck(vkCreateImageView(device, &viewInfo, nullptr, &view), "vkCreateImageView dmabuf");

    auto result = std::make_shared<VulkanImage>(device, image, memory, view, format,
                                                static_cast<int>(spec.width), static_cast<int>(spec.height));
    releaseSharedVulkanCore();
    acquiredCoreReference = false;
    return result;
  } catch (...) {
    if (fd >= 0) close(fd);
    if (view) vkDestroyImageView(device, view, nullptr);
    if (memory) vkFreeMemory(device, memory, nullptr);
    if (image) vkDestroyImage(device, image, nullptr);
    if (acquiredCoreReference) releaseSharedVulkanCore();
    throw;
  }
}

std::shared_ptr<Image> Image::fromRgbaPixels(std::uint32_t width, std::uint32_t height,
                                             std::span<std::uint8_t const> rgbaPixels, void*) {
  return fromPixels(width, height, rgbaPixels, PixelFormat::Rgba8888, nullptr);
}

std::shared_ptr<Image> Image::fromPixels(std::uint32_t width,
                                         std::uint32_t height,
                                         std::span<std::uint8_t const> pixels,
                                         PixelFormat pixelFormat,
                                         void*) {
  std::size_t const expectedSize = static_cast<std::size_t>(width) * height * 4u;
  if (width == 0 || height == 0 || pixels.size() != expectedSize) {
    return nullptr;
  }

  std::vector<std::uint8_t> copy(expectedSize);
  std::memcpy(copy.data(), pixels.data(), expectedSize);
  return std::make_shared<VulkanImage>(static_cast<int>(width),
                                       static_cast<int>(height),
                                       std::move(copy),
                                       vkFormatForImagePixelFormat(pixelFormat));
}

std::shared_ptr<Image> rasterizeToImage(Canvas &canvas, Size logicalSize, RasterizeDrawCallback draw, float dpiScale) {
  auto *vulkan = dynamic_cast<VulkanCanvas *>(&canvas);
  if (!vulkan)
    return nullptr;
  return vulkan->rasterize(logicalSize, draw, dpiScale);
}

namespace detail {

VkInstance vulkanContextInstance() noexcept {
  std::lock_guard lock(gVulkanCoreMutex);
  return gVulkanCore.instance;
}

VkPhysicalDevice vulkanContextPhysicalDevice() noexcept {
  std::lock_guard lock(gVulkanCoreMutex);
  return gVulkanCore.physical;
}

VkDevice vulkanContextDevice() noexcept {
  std::lock_guard lock(gVulkanCoreMutex);
  return gVulkanCore.device;
}

std::uint32_t vulkanContextQueueFamily() noexcept {
  std::lock_guard lock(gVulkanCoreMutex);
  return gVulkanCore.queueFamily;
}

VkQueue vulkanContextQueue() noexcept {
  std::lock_guard lock(gVulkanCoreMutex);
  return gVulkanCore.queue;
}

VmaAllocator vulkanContextAllocator() noexcept {
  std::lock_guard lock(gVulkanCoreMutex);
  return gVulkanCore.allocator;
}

VkFormat vulkanContextPreferredColorFormat() noexcept {
  std::lock_guard lock(gVulkanCoreMutex);
  if (gVulkanCore.resources.renderFormat != VK_FORMAT_UNDEFINED) {
    return gVulkanCore.resources.renderFormat;
  }
  return VK_FORMAT_B8G8R8A8_UNORM;
}

void vulkanContextAddRequiredInstanceExtension(char const *name) {
  std::lock_guard lock(gVulkanCoreMutex);
  if (gVulkanCore.instance) {
    throw std::runtime_error("Cannot add Vulkan instance extension after instance creation");
  }
  appendUniqueExtension(gRequiredInstanceExtensions, name);
}

void vulkanContextAddRequiredDeviceExtension(char const *name) {
  std::lock_guard lock(gVulkanCoreMutex);
  if (gVulkanCore.device) {
    throw std::runtime_error("Cannot add Vulkan device extension after device creation");
  }
  appendUniqueExtension(gRequiredDeviceExtensions, name);
}

void vulkanContextEnsureInitialized() {
  ensureSharedVulkanInstanceImpl();
  static bool holdsReference = false;
  if (!holdsReference) {
    acquireSharedVulkanCore(VK_NULL_HANDLE);
    holdsReference = true;
  }
}

} // namespace detail

} // namespace lambdaui
