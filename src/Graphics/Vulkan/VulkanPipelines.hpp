#pragma once

#include "Graphics/Vulkan/VulkanCanvasShared.hpp"

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace lambdaui {

struct VulkanCommandObjectsContext {
  VkDevice device;
  std::uint32_t queueFamily;
  VkCommandPool& commandPool;
  std::array<VkCommandBuffer, kVulkanMaxFramesInFlight>& commandBuffers;
  std::array<VkSemaphore, kVulkanMaxFramesInFlight>& imageAvailable;
  std::array<VkFence, kVulkanMaxFramesInFlight>& frameFences;
};

struct VulkanResetFrameFenceContext {
  VkDevice device;
  std::size_t& resetFrameFenceIndex;
  std::size_t noResetFrameFence;
  std::array<VkFence, kVulkanMaxFramesInFlight>& frameFences;
  std::vector<VkFence>& imageInFlightFences;
};

class VulkanPipelines {
public:
  void createCommandObjects(VulkanCommandObjectsContext& context) const;
  void recoverResetFrameFence(VulkanResetFrameFenceContext& context) const;
  VkSurfaceFormatKHR chooseSurfaceFormat(VkPhysicalDevice physical, VkSurfaceKHR surface) const;
  void createDescriptors(VkDevice device, SharedVulkanCore::Resources& resources) const;
  void createSampler(VkDevice device, SharedVulkanCore::Resources& resources) const;
  void createPipelines(VkDevice device, SharedVulkanCore::Resources& resources) const;

private:
  struct VertexInput {
    VkVertexInputBindingDescription const* bindings = nullptr;
    std::uint32_t bindingCount = 0;
    VkVertexInputAttributeDescription const* attrs = nullptr;
    std::uint32_t attrCount = 0;
  };

  VkDescriptorSetLayout createStorageLayout(VkDevice device) const;
  VkPipelineLayout createPipelineLayout(VkDevice device,
                                        std::initializer_list<VkDescriptorSetLayout> layouts,
                                        bool viewportPush) const;
  VkPipeline createPipeline(VkDevice device,
                            SharedVulkanCore::Resources const& resources,
                            VkPipelineLayout layout,
                            unsigned char const* vertBytes,
                            unsigned int vertLen,
                            unsigned char const* fragBytes,
                            unsigned int fragLen,
                            VertexInput input,
                            VkFormat colorFormatOverride = VK_FORMAT_UNDEFINED) const;
};

} // namespace lambdaui
