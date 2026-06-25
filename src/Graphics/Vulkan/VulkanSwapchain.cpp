#include "Graphics/Vulkan/VulkanSwapchain.hpp"

#include "Detail/ResizeTrace.hpp"
#include "Graphics/Vulkan/VulkanCheck.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <utility>

namespace lambdaui {

namespace {

void destroyRetiredSwapchainResources(VkDevice device, RetiredSwapchain& retired) {
  for (VkSemaphore semaphore : retired.renderFinished) {
    if (semaphore) {
      vkDestroySemaphore(device, semaphore, nullptr);
    }
  }
  retired.renderFinished.clear();
  for (VkFence fence : retired.presentFences) {
    if (fence) {
      vkDestroyFence(device, fence, nullptr);
    }
  }
  retired.presentFences.clear();
  for (VkImageView view : retired.views) {
    if (view) {
      vkDestroyImageView(device, view, nullptr);
    }
  }
  retired.views.clear();
  if (retired.swapchain) {
    vkDestroySwapchainKHR(device, retired.swapchain, nullptr);
    retired.swapchain = VK_NULL_HANDLE;
  }
}

bool retiredPresentFencesReady(VkDevice device, RetiredSwapchain const& retired) {
  for (VkFence fence : retired.presentFences) {
    if (!fence) {
      continue;
    }
    VkResult const status = vkGetFenceStatus(device, fence);
    if (status == VK_NOT_READY) {
      return false;
    }
    vkCheck(status, "vkGetFenceStatus(retiredPresentFence)");
  }
  return true;
}

bool retiredFrameFencesReady(RetiredSwapchain const& retired,
                             std::span<std::uint64_t const> frameFenceCompleteGenerations) {
  for (std::size_t i = 0; i < frameFenceCompleteGenerations.size(); ++i) {
    if (frameFenceCompleteGenerations[i] < retired.frameFenceGenerations[i]) {
      return false;
    }
  }
  return true;
}

bool retiredSwapchainReady(VkDevice device,
                           RetiredSwapchain const& retired,
                           std::span<std::uint64_t const> frameFenceCompleteGenerations,
                           bool force) {
  if (force) {
    return true;
  }
  if (!retired.presentFences.empty() && !retiredPresentFencesReady(device, retired)) {
    return false;
  }
  return retiredFrameFencesReady(retired, frameFenceCompleteGenerations);
}

VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported, bool transparentSurface) {
  auto supports = [supported](VkCompositeAlphaFlagBitsKHR mode) {
    return (supported & mode) == mode;
  };
  if (!transparentSurface) {
    if (supports(VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
      return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }
    if (supports(VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)) {
      return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    }
  } else {
    if (supports(VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)) {
      return VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    }
    if (supports(VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR)) {
      return VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    }
  }
  if (supports(VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)) {
    return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
  }
  return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

} // namespace

void VulkanSwapchain::markFrameFenceComplete(std::span<std::uint64_t const> submitGenerations,
                                             std::span<std::uint64_t> completeGenerations,
                                             std::size_t index) const {
  if (index >= completeGenerations.size() || index >= submitGenerations.size()) {
    return;
  }
  completeGenerations[index] = submitGenerations[index];
}

void VulkanSwapchain::markFrameFenceComplete(std::span<VkFence const> frameFences,
                                             std::span<std::uint64_t const> submitGenerations,
                                             std::span<std::uint64_t> completeGenerations,
                                             VkFence fence) const {
  if (!fence) {
    return;
  }
  for (std::size_t i = 0; i < frameFences.size(); ++i) {
    if (frameFences[i] == fence) {
      markFrameFenceComplete(submitGenerations, completeGenerations, i);
      return;
    }
  }
}

void VulkanSwapchain::markFrameFenceSubmitted(std::span<std::uint64_t> submitGenerations,
                                              std::size_t index) const {
  if (index >= submitGenerations.size()) {
    return;
  }
  ++submitGenerations[index];
}

VkFence VulkanSwapchain::createPresentFence(VkDevice device, bool signaled, char const* label) const {
  VkFence fence = VK_NULL_HANDLE;
  auto info = vkStructure<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
  if (signaled) {
    info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  }
  vkCheck(vkCreateFence(device, &info, nullptr, &fence), label);
  return fence;
}

void VulkanSwapchain::retireSwapchains(VkDevice device,
                                       std::vector<RetiredSwapchain>& retiredSwapchains,
                                       std::span<std::uint64_t const> frameFenceCompleteGenerations,
                                       bool force) const {
  for (auto it = retiredSwapchains.begin(); it != retiredSwapchains.end();) {
    if (retiredSwapchainReady(device, *it, frameFenceCompleteGenerations, force)) {
      destroyRetiredSwapchainResources(device, *it);
      it = retiredSwapchains.erase(it);
    } else {
      ++it;
    }
  }
}

void VulkanSwapchain::retireSwapchainResources(std::vector<RetiredSwapchain>& retiredSwapchains,
                                               std::span<std::uint64_t const> frameFenceSubmitGenerations,
                                               VkSwapchainKHR swapchain,
                                               std::vector<VkImageView> views,
                                               std::vector<VkSemaphore> renderFinished,
                                               std::vector<VkFence> presentFences) const {
  if (!swapchain && views.empty() && renderFinished.empty() && presentFences.empty()) {
    return;
  }
  RetiredSwapchain retired{};
  retired.swapchain = swapchain;
  retired.views = std::move(views);
  retired.renderFinished = std::move(renderFinished);
  retired.presentFences = std::move(presentFences);
  std::copy(frameFenceSubmitGenerations.begin(),
            frameFenceSubmitGenerations.end(),
            retired.frameFenceGenerations.begin());
  retiredSwapchains.push_back(std::move(retired));
}

void VulkanSwapchain::retirePresentFence(std::vector<RetiredSwapchain>& retiredSwapchains,
                                         VkFence fence) const {
  if (!fence) {
    return;
  }
  RetiredSwapchain retired{};
  retired.presentFences.push_back(fence);
  retiredSwapchains.push_back(std::move(retired));
}

void VulkanSwapchain::recreate(VulkanSwapchainRecreateContext& context) const {
  if (!context.device) {
    return;
  }
  auto const recreateStart = std::chrono::steady_clock::now();
  auto phaseStart = recreateStart;
  auto phaseMs = [&](std::chrono::steady_clock::time_point start) {
    return static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count()) / 1000.0;
  };
  VkSwapchainKHR oldSwapchain = context.swapchain;
  std::vector<VkImageView> oldViews = std::move(context.swapchainViews);
  std::vector<VkSemaphore> oldImageRenderFinished = std::move(context.imageRenderFinished);
  std::vector<VkFence> oldImagePresentFences = std::move(context.imagePresentFences);
  context.swapchain = VK_NULL_HANDLE;
  context.swapchainImages.clear();
  context.swapchainViews.clear();
  context.imageRenderFinished.clear();
  context.imagePresentFences.clear();
  retireSwapchainResources(context.retiredSwapchains,
                           context.frameFenceSubmitGenerations,
                           oldSwapchain,
                           std::move(oldViews),
                           std::move(oldImageRenderFinished),
                           std::move(oldImagePresentFences));
  double const retireMs = phaseMs(phaseStart);
  phaseStart = std::chrono::steady_clock::now();
  VkSurfaceCapabilitiesKHR caps{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context.physical, context.surface, &caps);
  std::uint32_t presentCount = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(context.physical, context.surface, &presentCount, nullptr);
  std::vector<VkPresentModeKHR> modes(presentCount);
  vkGetPhysicalDeviceSurfacePresentModesKHR(context.physical, context.surface, &presentCount, modes.data());
  double const capsMs = phaseMs(phaseStart);
  phaseStart = std::chrono::steady_clock::now();
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
  if (!context.forceFifoPresentMode) {
    for (auto candidate : modes) {
      if (candidate == VK_PRESENT_MODE_MAILBOX_KHR) {
        mode = candidate;
        break;
      }
    }
  }
  context.presentMode = mode;
  std::uint32_t const requestedW = static_cast<std::uint32_t>(
      std::max({1, context.framebufferWidth, context.swapchainTargetWidth}));
  std::uint32_t const requestedH = static_cast<std::uint32_t>(
      std::max({1, context.framebufferHeight, context.swapchainTargetHeight}));
  VkExtent2D const requestedExtent{
      std::clamp(requestedW, caps.minImageExtent.width, caps.maxImageExtent.width),
      std::clamp(requestedH, caps.minImageExtent.height, caps.maxImageExtent.height),
  };
  if (caps.currentExtent.width != UINT32_MAX) {
    context.swapExtent = caps.currentExtent;
  } else {
    context.swapExtent = requestedExtent;
  }
  if (detail::resizeTraceEnabled()) {
    std::fprintf(stderr,
                 "Lambda Vulkan: swapchain extent window=%u framebuffer=%dx%d target=%dx%d "
                 "caps current=%ux%u min=%ux%u max=%ux%u requested=%ux%u chosen=%ux%u\n",
                 context.handle,
                 context.framebufferWidth,
                 context.framebufferHeight,
                 context.swapchainTargetWidth,
                 context.swapchainTargetHeight,
                 caps.currentExtent.width,
                 caps.currentExtent.height,
                 caps.minImageExtent.width,
                 caps.minImageExtent.height,
                 caps.maxImageExtent.width,
                 caps.maxImageExtent.height,
                 requestedExtent.width,
                 requestedExtent.height,
                 context.swapExtent.width,
                 context.swapExtent.height);
  }
  std::uint32_t const preferredImageCount =
      std::max<std::uint32_t>(caps.minImageCount + 2u, static_cast<std::uint32_t>(kVulkanMaxFramesInFlight));
  std::uint32_t imageCount = std::clamp(preferredImageCount,
                                        caps.minImageCount,
                                        caps.maxImageCount ? caps.maxImageCount : preferredImageCount);
  auto info = vkStructure<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
  info.surface = context.surface;
  info.minImageCount = imageCount;
  info.imageFormat = context.surfaceFormat.format;
  info.imageColorSpace = context.surfaceFormat.colorSpace;
  info.imageExtent = context.swapExtent;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = caps.currentTransform;
  info.compositeAlpha = chooseCompositeAlpha(caps.supportedCompositeAlpha, context.transparentSurface);
  info.presentMode = context.presentMode;
  info.clipped = VK_TRUE;
  info.oldSwapchain = oldSwapchain;
  vkCheck(vkCreateSwapchainKHR(context.device, &info, nullptr, &context.swapchain), "vkCreateSwapchainKHR");
  double const createMs = phaseMs(phaseStart);
  phaseStart = std::chrono::steady_clock::now();
  std::uint32_t count = 0;
  vkGetSwapchainImagesKHR(context.device, context.swapchain, &count, nullptr);
  context.swapchainImages.resize(count);
  vkGetSwapchainImagesKHR(context.device, context.swapchain, &count, context.swapchainImages.data());
  context.imageInFlightFences.assign(count, VK_NULL_HANDLE);
  context.imageRenderFinished.resize(count, VK_NULL_HANDLE);
  if (context.shared && context.shared->swapchainMaintenance1 && context.presentFencesEnabled &&
      !context.presentFenceRuntimeDisabled) {
    context.imagePresentFences.resize(count, VK_NULL_HANDLE);
  }
  context.swapchainViews.resize(count);
  auto sem = vkStructure<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
  for (std::size_t i = 0; i < context.swapchainImages.size(); ++i) {
    vkCheck(vkCreateSemaphore(context.device, &sem, nullptr, &context.imageRenderFinished[i]),
            "vkCreateSemaphore");
    if (!context.imagePresentFences.empty()) {
      context.imagePresentFences[i] = createPresentFence(context.device, true, "vkCreateFence(presentFence)");
    }
    auto view = vkStructure<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
    view.image = context.swapchainImages[i];
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = context.surfaceFormat.format;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    vkCheck(vkCreateImageView(context.device, &view, nullptr, &context.swapchainViews[i]),
            "vkCreateImageView");
  }
  double const imageSetupMs = phaseMs(phaseStart);
  if (detail::resizeTraceEnabled()) {
    auto const elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - recreateStart).count();
    LAMBDA_RESIZE_TRACE(
        "vulkan-recreate-swapchain",
        "window=%u framebuffer=%dx%d target=%dx%d extent=%ux%u images=%zu "
        "capsCurrent=%ux%u requested=%ux%u "
        "presentFences=%zu retire=%zu retireQueue=%.3fms caps=%.3fms create=%.3fms imageSetup=%.3fms "
        "elapsed=%.3fms\n",
        context.handle,
        context.framebufferWidth,
        context.framebufferHeight,
        context.swapchainTargetWidth,
        context.swapchainTargetHeight,
        context.swapExtent.width,
        context.swapExtent.height,
        context.swapchainImages.size(),
        caps.currentExtent.width,
        caps.currentExtent.height,
        requestedExtent.width,
        requestedExtent.height,
        context.imagePresentFences.size(),
        context.retiredSwapchains.size(),
        retireMs,
        capsMs,
        createMs,
        imageSetupMs,
        static_cast<double>(elapsed) / 1000.0);
  }
}

void VulkanSwapchain::destroy(VulkanSwapchainDestroyContext& context) const {
  retireSwapchainResources(context.retiredSwapchains,
                           context.frameFenceSubmitGenerations,
                           context.swapchain,
                           std::move(context.swapchainViews),
                           std::move(context.imageRenderFinished),
                           std::move(context.imagePresentFences));
  context.swapchain = VK_NULL_HANDLE;
  context.swapchainViews.clear();
  context.swapchainImages.clear();
  context.imageInFlightFences.clear();
  context.imageRenderFinished.clear();
  context.imagePresentFences.clear();
  retireSwapchains(context.device, context.retiredSwapchains, context.frameFenceCompleteGenerations, true);
}

} // namespace lambdaui
