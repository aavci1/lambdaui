#pragma once

#include "Graphics/Vulkan/VulkanCanvasShared.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace lambdaui {

struct VulkanSwapchainRecreateContext {
  VkDevice device;
  VkPhysicalDevice physical;
  VkSurfaceKHR surface;
  SharedVulkanCore* shared;
  unsigned int handle;
  int framebufferWidth;
  int framebufferHeight;
  int swapchainTargetWidth;
  int swapchainTargetHeight;
  VkSurfaceFormatKHR surfaceFormat;
  bool forceFifoPresentMode;
  bool presentFencesEnabled;
  bool presentFenceRuntimeDisabled;
  bool transparentSurface;
  VkSwapchainKHR& swapchain;
  VkPresentModeKHR& presentMode;
  VkExtent2D& swapExtent;
  std::vector<VkImage>& swapchainImages;
  std::vector<VkImageView>& swapchainViews;
  std::vector<VkFence>& imageInFlightFences;
  std::vector<VkSemaphore>& imageRenderFinished;
  std::vector<VkFence>& imagePresentFences;
  std::vector<RetiredSwapchain>& retiredSwapchains;
  std::array<std::uint64_t, kVulkanMaxFramesInFlight> const& frameFenceSubmitGenerations;
};

struct VulkanSwapchainDestroyContext {
  VkDevice device;
  VkSwapchainKHR& swapchain;
  std::vector<VkImage>& swapchainImages;
  std::vector<VkImageView>& swapchainViews;
  std::vector<VkFence>& imageInFlightFences;
  std::vector<VkSemaphore>& imageRenderFinished;
  std::vector<VkFence>& imagePresentFences;
  std::vector<RetiredSwapchain>& retiredSwapchains;
  std::array<std::uint64_t, kVulkanMaxFramesInFlight> const& frameFenceSubmitGenerations;
  std::array<std::uint64_t, kVulkanMaxFramesInFlight> const& frameFenceCompleteGenerations;
};

class VulkanSwapchain {
public:
  void markFrameFenceComplete(std::span<std::uint64_t const> submitGenerations,
                              std::span<std::uint64_t> completeGenerations,
                              std::size_t index) const;
  void markFrameFenceComplete(std::span<VkFence const> frameFences,
                              std::span<std::uint64_t const> submitGenerations,
                              std::span<std::uint64_t> completeGenerations,
                              VkFence fence) const;
  void markFrameFenceSubmitted(std::span<std::uint64_t> submitGenerations,
                               std::size_t index) const;
  VkFence createPresentFence(VkDevice device, bool signaled, char const* label) const;
  void retireSwapchains(VkDevice device,
                        std::vector<RetiredSwapchain>& retiredSwapchains,
                        std::span<std::uint64_t const> frameFenceCompleteGenerations,
                        bool force) const;
  void retireSwapchainResources(std::vector<RetiredSwapchain>& retiredSwapchains,
                                std::span<std::uint64_t const> frameFenceSubmitGenerations,
                                VkSwapchainKHR swapchain,
                                std::vector<VkImageView> views,
                                std::vector<VkSemaphore> renderFinished,
                                std::vector<VkFence> presentFences) const;
  void retirePresentFence(std::vector<RetiredSwapchain>& retiredSwapchains, VkFence fence) const;
  void recreate(VulkanSwapchainRecreateContext& context) const;
  void destroy(VulkanSwapchainDestroyContext& context) const;
};

} // namespace lambdaui
