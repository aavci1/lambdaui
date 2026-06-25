#pragma once

#include "Graphics/Vulkan/VulkanCanvasShared.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace lambdaui {

struct VulkanCommandState {
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorSet descriptor0 = VK_NULL_HANDLE;
  VkDescriptorSet descriptor1 = VK_NULL_HANDLE;
  VkBuffer vertexBuffer = VK_NULL_HANDLE;
  bool pushConstantsValid = false;
  float translationX = 0.f;
  float translationY = 0.f;
};

struct VulkanDrawCommandContext {
  float viewportWidth = 0.f;
  float viewportHeight = 0.f;
  SharedVulkanCore::Resources const* resources = nullptr;
  FrameGeometryResources const* geometry = nullptr;
};

void drawVulkanRectRange(VkCommandBuffer commandBuffer,
                         VulkanCommandState& state,
                         VulkanDrawCommandContext const& context,
                         std::uint32_t first,
                         std::uint32_t count,
                         VkDescriptorSet descriptor = VK_NULL_HANDLE,
                         float translationX = 0.f,
                         float translationY = 0.f);

void drawVulkanCalloutRange(VkCommandBuffer commandBuffer,
                            VulkanCommandState& state,
                            VulkanDrawCommandContext const& context,
                            std::uint32_t first,
                            std::uint32_t count,
                            float translationX = 0.f,
                            float translationY = 0.f);

void drawVulkanPathRange(VkCommandBuffer commandBuffer,
                         VulkanCommandState& state,
                         VulkanDrawCommandContext const& context,
                         std::uint32_t first,
                         std::uint32_t count,
                         VkBuffer vertexBuffer = VK_NULL_HANDLE,
                         float translationX = 0.f,
                         float translationY = 0.f);

void drawVulkanImageRange(VkCommandBuffer commandBuffer,
                          VulkanCommandState& state,
                          VulkanDrawCommandContext const& context,
                          Texture* texture,
                          std::uint32_t first,
                          std::uint32_t count,
                          VkDescriptorSet descriptor = VK_NULL_HANDLE,
                          float translationX = 0.f,
                          float translationY = 0.f,
                          bool premultipliedAlpha = false);

void drawVulkanBackdropRange(VkCommandBuffer commandBuffer,
                             VulkanCommandState& state,
                             VulkanDrawCommandContext const& context,
                             Texture* texture,
                             std::uint32_t first,
                             std::uint32_t count,
                             VkDescriptorSet descriptor = VK_NULL_HANDLE,
                             float translationX = 0.f,
                             float translationY = 0.f);

void drawVulkanBackdropBlurPass(VkCommandBuffer commandBuffer,
                                VulkanCommandState& state,
                                VulkanDrawCommandContext const& context,
                                Texture* texture,
                                std::uint32_t first);

} // namespace lambdaui
