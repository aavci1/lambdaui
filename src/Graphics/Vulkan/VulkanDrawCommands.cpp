#include "Graphics/Vulkan/VulkanDrawCommands.hpp"

#include <cmath>

namespace lambdaui {

namespace {

VulkanDrawPushConstants drawPushConstants(VulkanDrawCommandContext const& context,
                                          float translationX = 0.f,
                                          float translationY = 0.f) {
  VulkanDrawPushConstants push{};
  push.viewport[0] = context.viewportWidth;
  push.viewport[1] = context.viewportHeight;
  push.translation[0] = translationX;
  push.translation[1] = translationY;
  return push;
}

void bindPipeline(VkCommandBuffer commandBuffer,
                  VulkanCommandState& state,
                  VkPipeline pipeline,
                  VkPipelineLayout layout) {
  if (state.pipeline != pipeline) {
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    state.pipeline = pipeline;
  }
  if (state.layout != layout) {
    state.layout = layout;
    state.descriptor0 = VK_NULL_HANDLE;
    state.descriptor1 = VK_NULL_HANDLE;
    state.pushConstantsValid = false;
  }
}

void bindDescriptorSet(VkCommandBuffer commandBuffer,
                       VulkanCommandState& state,
                       VkPipelineLayout layout,
                       std::uint32_t index,
                       VkDescriptorSet descriptor) {
  VkDescriptorSet* cached = index == 0 ? &state.descriptor0 : &state.descriptor1;
  if (*cached == descriptor) {
    return;
  }
  vkCmdBindDescriptorSets(commandBuffer,
                          VK_PIPELINE_BIND_POINT_GRAPHICS,
                          layout,
                          index,
                          1,
                          &descriptor,
                          0,
                          nullptr);
  *cached = descriptor;
}

void pushDrawConstants(VkCommandBuffer commandBuffer,
                       VulkanCommandState& state,
                       VulkanDrawCommandContext const& context,
                       VkPipelineLayout layout,
                       float translationX = 0.f,
                       float translationY = 0.f) {
  if (state.pushConstantsValid &&
      std::abs(state.translationX - translationX) <= 1e-4f &&
      std::abs(state.translationY - translationY) <= 1e-4f) {
    return;
  }
  VulkanDrawPushConstants const push = drawPushConstants(context, translationX, translationY);
  vkCmdPushConstants(commandBuffer, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
  state.pushConstantsValid = true;
  state.translationX = translationX;
  state.translationY = translationY;
}

void bindVertexBuffer(VkCommandBuffer commandBuffer, VulkanCommandState& state, VkBuffer buffer) {
  if (state.vertexBuffer == buffer) {
    return;
  }
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(commandBuffer, 0, 1, &buffer, &offset);
  state.vertexBuffer = buffer;
}

} // namespace

void drawVulkanRectRange(VkCommandBuffer commandBuffer,
                         VulkanCommandState& state,
                         VulkanDrawCommandContext const& context,
                         std::uint32_t first,
                         std::uint32_t count,
                         VkDescriptorSet descriptor,
                         float translationX,
                         float translationY) {
  if (count == 0 || !context.resources || !context.geometry) {
    return;
  }
  SharedVulkanCore::Resources const& res = *context.resources;
  FrameGeometryResources const& geometry = *context.geometry;
  VkDescriptorSet const storageDescriptor = descriptor ? descriptor : geometry.rectDescriptorSet;
  if (!storageDescriptor) {
    return;
  }
  bindPipeline(commandBuffer, state, res.rectPipeline, res.rectPipelineLayout);
  bindDescriptorSet(commandBuffer, state, res.rectPipelineLayout, 0, storageDescriptor);
  pushDrawConstants(commandBuffer, state, context, res.rectPipelineLayout, translationX, translationY);
  vkCmdDraw(commandBuffer, 6, count, 0, first);
}

void drawVulkanCalloutRange(VkCommandBuffer commandBuffer,
                            VulkanCommandState& state,
                            VulkanDrawCommandContext const& context,
                            std::uint32_t first,
                            std::uint32_t count,
                            float translationX,
                            float translationY) {
  if (count == 0 || !context.resources || !context.geometry) {
    return;
  }
  SharedVulkanCore::Resources const& res = *context.resources;
  FrameGeometryResources const& geometry = *context.geometry;
  if (!geometry.calloutDescriptorSet) {
    return;
  }
  bindPipeline(commandBuffer, state, res.calloutPipeline, res.calloutPipelineLayout);
  bindDescriptorSet(commandBuffer, state, res.calloutPipelineLayout, 0, geometry.calloutDescriptorSet);
  pushDrawConstants(commandBuffer, state, context, res.calloutPipelineLayout, translationX, translationY);
  vkCmdDraw(commandBuffer, 6, count, 0, first);
}

void drawVulkanPathRange(VkCommandBuffer commandBuffer,
                         VulkanCommandState& state,
                         VulkanDrawCommandContext const& context,
                         std::uint32_t first,
                         std::uint32_t count,
                         VkBuffer vertexBuffer,
                         float translationX,
                         float translationY) {
  if (count == 0 || !context.resources || !context.geometry) {
    return;
  }
  FrameGeometryResources const& geometry = *context.geometry;
  VkBuffer const buffer = vertexBuffer ? vertexBuffer : geometry.pathBuffer.buffer;
  if (!buffer) {
    return;
  }
  SharedVulkanCore::Resources const& res = *context.resources;
  bindPipeline(commandBuffer, state, res.pathPipeline, res.pathPipelineLayout);
  pushDrawConstants(commandBuffer, state, context, res.pathPipelineLayout, translationX, translationY);
  bindVertexBuffer(commandBuffer, state, buffer);
  vkCmdDraw(commandBuffer, count, 1, first, 0);
}

void drawVulkanImageRange(VkCommandBuffer commandBuffer,
                          VulkanCommandState& state,
                          VulkanDrawCommandContext const& context,
                          Texture* texture,
                          std::uint32_t first,
                          std::uint32_t count,
                          VkDescriptorSet descriptor,
                          float translationX,
                          float translationY,
                          bool premultipliedAlpha) {
  if (!texture || !texture->descriptor || count == 0 || !context.resources || !context.geometry) {
    return;
  }
  SharedVulkanCore::Resources const& res = *context.resources;
  FrameGeometryResources const& geometry = *context.geometry;
  VkDescriptorSet const storageDescriptor = descriptor ? descriptor : geometry.quadDescriptorSet;
  if (!storageDescriptor) {
    return;
  }
  bindPipeline(commandBuffer,
               state,
               premultipliedAlpha ? res.imageUnpremultiplyPipeline : res.imagePipeline,
               res.imagePipelineLayout);
  bindDescriptorSet(commandBuffer, state, res.imagePipelineLayout, 0, storageDescriptor);
  pushDrawConstants(commandBuffer, state, context, res.imagePipelineLayout, translationX, translationY);
  bindDescriptorSet(commandBuffer, state, res.imagePipelineLayout, 1, texture->descriptor);
  vkCmdDraw(commandBuffer, 6, count, 0, first);
}

void drawVulkanBackdropRange(VkCommandBuffer commandBuffer,
                             VulkanCommandState& state,
                             VulkanDrawCommandContext const& context,
                             Texture* texture,
                             std::uint32_t first,
                             std::uint32_t count,
                             VkDescriptorSet descriptor,
                             float translationX,
                             float translationY) {
  if (!texture || !texture->descriptor || count == 0 || !context.resources || !context.geometry) {
    return;
  }
  SharedVulkanCore::Resources const& res = *context.resources;
  FrameGeometryResources const& geometry = *context.geometry;
  VkDescriptorSet const storageDescriptor = descriptor ? descriptor : geometry.quadDescriptorSet;
  if (!storageDescriptor) {
    return;
  }
  bindPipeline(commandBuffer, state, res.backdropPipeline, res.backdropPipelineLayout);
  bindDescriptorSet(commandBuffer, state, res.backdropPipelineLayout, 0, storageDescriptor);
  pushDrawConstants(commandBuffer, state, context, res.backdropPipelineLayout, translationX, translationY);
  bindDescriptorSet(commandBuffer, state, res.backdropPipelineLayout, 1, texture->descriptor);
  vkCmdDraw(commandBuffer, 6, count, 0, first);
}

void drawVulkanBackdropBlurPass(VkCommandBuffer commandBuffer,
                                VulkanCommandState& state,
                                VulkanDrawCommandContext const& context,
                                Texture* texture,
                                std::uint32_t first) {
  if (!texture || !texture->descriptor || !context.resources || !context.geometry) {
    return;
  }
  SharedVulkanCore::Resources const& res = *context.resources;
  FrameGeometryResources const& geometry = *context.geometry;
  if (!geometry.quadDescriptorSet) {
    return;
  }
  bindPipeline(commandBuffer, state, res.backdropBlurPipeline, res.backdropPipelineLayout);
  bindDescriptorSet(commandBuffer, state, res.backdropPipelineLayout, 0, geometry.quadDescriptorSet);
  pushDrawConstants(commandBuffer, state, context, res.backdropPipelineLayout);
  bindDescriptorSet(commandBuffer, state, res.backdropPipelineLayout, 1, texture->descriptor);
  vkCmdDraw(commandBuffer, 6, 1, 0, first);
}

} // namespace lambdaui
