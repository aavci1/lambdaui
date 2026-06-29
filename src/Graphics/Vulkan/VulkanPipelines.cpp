#include "Graphics/Vulkan/VulkanPipelines.hpp"

#include "Graphics/Vulkan/VulkanCanvasTypes.hpp"
#include "Graphics/Vulkan/VulkanCheck.hpp"
#include "Graphics/Vulkan/generated/backdrop_blur_frag_spv.hpp"
#include "Graphics/Vulkan/generated/backdrop_frag_spv.hpp"
#include "Graphics/Vulkan/generated/callout_frag_spv.hpp"
#include "Graphics/Vulkan/generated/callout_vert_spv.hpp"
#include "Graphics/Vulkan/generated/image_frag_spv.hpp"
#include "Graphics/Vulkan/generated/image_unpremultiply_frag_spv.hpp"
#include "Graphics/Vulkan/generated/image_vert_spv.hpp"
#include "Graphics/Vulkan/generated/path_frag_spv.hpp"
#include "Graphics/Vulkan/generated/path_vert_spv.hpp"
#include "Graphics/Vulkan/generated/rect_frag_spv.hpp"
#include "Graphics/Vulkan/generated/rect_vert_spv.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace lambdaui {

namespace {

constexpr std::uint32_t kDescriptorPoolStorageBufferSets = 4096;
constexpr std::uint32_t kDescriptorPoolImageSamplerSets = 8192;

VkShaderModule shaderModule(VkDevice device, unsigned char const* bytes, unsigned int len) {
  auto info = vkStructure<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
  info.codeSize = len;
  info.pCode = reinterpret_cast<std::uint32_t const*>(bytes);
  VkShaderModule module = VK_NULL_HANDLE;
  vkCheck(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule");
  return module;
}

void setSrcOverBlend(VkPipelineColorBlendAttachmentState& blend) {
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
}

void applyBlendModeToAttachment(VkPipelineColorBlendAttachmentState& blend, BlendMode mode) {
  setSrcOverBlend(blend);
  switch (mode) {
  case BlendMode::Normal:
  case BlendMode::SrcOver:
    return;
  case BlendMode::Multiply:
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    return;
  case BlendMode::Screen:
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    return;
  case BlendMode::Darken:
    blend.colorBlendOp = VK_BLEND_OP_MIN;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    return;
  case BlendMode::Lighten:
    blend.colorBlendOp = VK_BLEND_OP_MAX;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    return;
  case BlendMode::Clear:
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    return;
  case BlendMode::Src:
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    return;
  case BlendMode::Dst:
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    return;
  case BlendMode::DstOver:
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    return;
  case BlendMode::SrcIn:
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    return;
  case BlendMode::DstIn:
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    return;
  case BlendMode::SrcOut:
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    return;
  case BlendMode::DstOut:
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    return;
  default:
    return;
  }
}

bool shouldCreateBlendPipeline(BlendMode mode) {
  switch (mode) {
  case BlendMode::Normal:
  case BlendMode::Multiply:
  case BlendMode::Screen:
  case BlendMode::Darken:
  case BlendMode::Lighten:
  case BlendMode::Clear:
  case BlendMode::Src:
  case BlendMode::Dst:
  case BlendMode::DstOver:
  case BlendMode::SrcIn:
  case BlendMode::DstIn:
  case BlendMode::SrcOut:
  case BlendMode::DstOut:
    return true;
  default:
    return false;
  }
}

} // namespace

void VulkanPipelines::createCommandObjects(VulkanCommandObjectsContext& context) const {
  auto pool = vkStructure<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = context.queueFamily;
  vkCheck(vkCreateCommandPool(context.device, &pool, nullptr, &context.commandPool), "vkCreateCommandPool");
  auto alloc = vkStructure<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
  alloc.commandPool = context.commandPool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = static_cast<std::uint32_t>(context.commandBuffers.size());
  vkCheck(vkAllocateCommandBuffers(context.device, &alloc, context.commandBuffers.data()),
          "vkAllocateCommandBuffers");
  auto sem = vkStructure<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
  auto fence = vkStructure<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
  fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  for (std::size_t i = 0; i < context.frameFences.size(); ++i) {
    vkCheck(vkCreateSemaphore(context.device, &sem, nullptr, &context.imageAvailable[i]), "vkCreateSemaphore");
    vkCheck(vkCreateFence(context.device, &fence, nullptr, &context.frameFences[i]), "vkCreateFence");
  }
}

void VulkanPipelines::recoverResetFrameFence(VulkanResetFrameFenceContext& context) const {
  if (context.resetFrameFenceIndex == context.noResetFrameFence ||
      context.resetFrameFenceIndex >= context.frameFences.size()) {
    return;
  }
  VkFence oldFence = context.frameFences[context.resetFrameFenceIndex];
  auto fence = vkStructure<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
  fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VkFence newFence = VK_NULL_HANDLE;
  if (vkCreateFence(context.device, &fence, nullptr, &newFence) == VK_SUCCESS) {
    for (VkFence& imageFence : context.imageInFlightFences) {
      if (imageFence == oldFence) {
        imageFence = newFence;
      }
    }
    context.frameFences[context.resetFrameFenceIndex] = newFence;
    if (oldFence) {
      vkDestroyFence(context.device, oldFence, nullptr);
    }
  }
  context.resetFrameFenceIndex = context.noResetFrameFence;
}

VkSurfaceFormatKHR VulkanPipelines::chooseSurfaceFormat(VkPhysicalDevice physical, VkSurfaceKHR surface) const {
  std::uint32_t count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data());
  VkSurfaceFormatKHR selected =
      formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                      : formats[0];
  auto choose = [&](VkFormat format) {
    for (auto const& candidate : formats) {
      if (candidate.format == format) {
        selected = candidate;
        return true;
      }
    }
    return false;
  };
  if (choose(VK_FORMAT_B8G8R8A8_UNORM) || choose(VK_FORMAT_R8G8B8A8_UNORM) ||
      choose(VK_FORMAT_A8B8G8R8_UNORM_PACK32)) {
    return selected;
  }
  for (auto const& candidate : formats) {
    if (candidate.format == VK_FORMAT_B8G8R8A8_SRGB || candidate.format == VK_FORMAT_R8G8B8A8_SRGB ||
        candidate.format == VK_FORMAT_A8B8G8R8_SRGB_PACK32) {
      selected = candidate;
      break;
    }
  }
  return selected;
}

void VulkanPipelines::createDescriptors(VkDevice device, SharedVulkanCore::Resources& resources) const {
  VkDescriptorPoolSize sizes[2]{
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kDescriptorPoolStorageBufferSets},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kDescriptorPoolImageSamplerSets},
  };
  auto pool = vkStructure<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
  pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pool.maxSets = kDescriptorPoolStorageBufferSets + kDescriptorPoolImageSamplerSets;
  pool.poolSizeCount = 2;
  pool.pPoolSizes = sizes;
  vkCheck(vkCreateDescriptorPool(device, &pool, nullptr, &resources.descriptorPool),
          "vkCreateDescriptorPool");
  resources.rectDescriptorLayout = createStorageLayout(device);
  resources.quadDescriptorLayout = createStorageLayout(device);
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  auto layout = vkStructure<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
  layout.bindingCount = 1;
  layout.pBindings = &binding;
  vkCheck(vkCreateDescriptorSetLayout(device, &layout, nullptr, &resources.textureDescriptorLayout),
          "vkCreateDescriptorSetLayout");
}

VkDescriptorSetLayout VulkanPipelines::createStorageLayout(VkDevice device) const {
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  auto info = vkStructure<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
  info.bindingCount = 1;
  info.pBindings = &binding;
  VkDescriptorSetLayout out = VK_NULL_HANDLE;
  vkCheck(vkCreateDescriptorSetLayout(device, &info, nullptr, &out), "vkCreateDescriptorSetLayout");
  return out;
}

void VulkanPipelines::createSampler(VkDevice device, SharedVulkanCore::Resources& resources) const {
  auto sampler = vkStructure<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
  sampler.magFilter = VK_FILTER_LINEAR;
  sampler.minFilter = VK_FILTER_LINEAR;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  vkCheck(vkCreateSampler(device, &sampler, nullptr, &resources.sampler), "vkCreateSampler");
}

void VulkanPipelines::createPipelines(VkDevice device, SharedVulkanCore::Resources& resources) const {
  resources.rectPipelineLayout = createPipelineLayout(device, {resources.rectDescriptorLayout}, true);
  resources.calloutPipelineLayout = createPipelineLayout(device, {resources.rectDescriptorLayout}, true);
  resources.imagePipelineLayout =
      createPipelineLayout(device, {resources.quadDescriptorLayout, resources.textureDescriptorLayout}, true);
  resources.backdropPipelineLayout =
      createPipelineLayout(device, {resources.quadDescriptorLayout, resources.textureDescriptorLayout}, true);
  resources.pathPipelineLayout = createPipelineLayout(device, {}, true);
  for (std::size_t i = 0; i < kVulkanBlendModePipelineCount; ++i) {
    BlendMode const mode = static_cast<BlendMode>(i);
    if (!shouldCreateBlendPipeline(mode)) {
      continue;
    }
    resources.rectPipelines[i] = createPipeline(device,
                                                resources,
                                                resources.rectPipelineLayout,
                                                lambda_rect_vert_spv,
                                                lambda_rect_vert_spv_len,
                                                lambda_rect_frag_spv,
                                                lambda_rect_frag_spv_len,
                                                {},
                                                mode);
    resources.calloutPipelines[i] = createPipeline(device,
                                                   resources,
                                                   resources.calloutPipelineLayout,
                                                   lambda_callout_vert_spv,
                                                   lambda_callout_vert_spv_len,
                                                   lambda_callout_frag_spv,
                                                   lambda_callout_frag_spv_len,
                                                   {},
                                                   mode);
    resources.imagePipelines[i] = createPipeline(device,
                                                 resources,
                                                 resources.imagePipelineLayout,
                                                 lambda_image_vert_spv,
                                                 lambda_image_vert_spv_len,
                                                 lambda_image_frag_spv,
                                                 lambda_image_frag_spv_len,
                                                 {},
                                                 mode);
    resources.imageUnpremultiplyPipelines[i] = createPipeline(device,
                                                              resources,
                                                              resources.imagePipelineLayout,
                                                              lambda_image_vert_spv,
                                                              lambda_image_vert_spv_len,
                                                              lambda_image_unpremultiply_frag_spv,
                                                              lambda_image_unpremultiply_frag_spv_len,
                                                              {},
                                                              mode);
  }
  resources.backdropPipeline = createPipeline(device,
                                              resources,
                                              resources.backdropPipelineLayout,
                                              lambda_image_vert_spv,
                                              lambda_image_vert_spv_len,
                                              lambda_backdrop_frag_spv,
                                              lambda_backdrop_frag_spv_len,
                                              {});
  resources.backdropBlurPipeline = createPipeline(device,
                                                  resources,
                                                  resources.backdropPipelineLayout,
                                                  lambda_image_vert_spv,
                                                  lambda_image_vert_spv_len,
                                                  lambda_backdrop_blur_frag_spv,
                                                  lambda_backdrop_blur_frag_spv_len,
                                                  {},
                                                  BlendMode::SrcOver,
                                                  resources.backdropRenderFormat);
  std::array<VkVertexInputBindingDescription, 1> binding{};
  binding[0].binding = 0;
  binding[0].stride = sizeof(VulkanPathVertex);
  binding[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  std::array<VkVertexInputAttributeDescription, 20> attrs{};
  attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(VulkanPathVertex, x)};
  attrs[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VulkanPathVertex, color)};
  attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(VulkanPathVertex, viewport)};
  attrs[3] = {3, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(VulkanPathVertex, local)};
  attrs[4] = {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VulkanPathVertex, fill0)};
  attrs[5] = {5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VulkanPathVertex, fill1)};
  attrs[6] = {6, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VulkanPathVertex, fill2)};
  attrs[7] = {7, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VulkanPathVertex, fill3)};
  attrs[8] = {8, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VulkanPathVertex, stops)};
  attrs[9] = {9, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VulkanPathVertex, gradient)};
  attrs[10] = {10, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VulkanPathVertex, params)};
  attrs[11] = {11, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VulkanPathVertex, clipHeader)};
  for (std::uint32_t i = 0; i < kVulkanRoundedClipEntryCount; ++i) {
    attrs[12 + i] = {12 + i,
                     0,
                     VK_FORMAT_R32G32B32A32_SFLOAT,
                     static_cast<std::uint32_t>(offsetof(VulkanPathVertex, clipEntries) +
                                                i * sizeof(float) * 4u)};
  }
  for (std::size_t i = 0; i < kVulkanBlendModePipelineCount; ++i) {
    if (!shouldCreateBlendPipeline(static_cast<BlendMode>(i))) {
      continue;
    }
    resources.pathPipelines[i] =
        createPipeline(device,
                       resources,
                       resources.pathPipelineLayout,
                       lambda_path_vert_spv,
                       lambda_path_vert_spv_len,
                       lambda_path_frag_spv,
                       lambda_path_frag_spv_len,
                       {binding.data(), 1, attrs.data(), static_cast<std::uint32_t>(attrs.size())},
                       static_cast<BlendMode>(i));
  }
}

VkPipelineLayout VulkanPipelines::createPipelineLayout(VkDevice device,
                                                       std::initializer_list<VkDescriptorSetLayout> layouts,
                                                       bool viewportPush) const {
  std::vector<VkDescriptorSetLayout> setLayouts(layouts);
  VkPushConstantRange push{};
  push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  push.offset = 0;
  push.size = sizeof(VulkanDrawPushConstants);
  auto info = vkStructure<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
  info.setLayoutCount = static_cast<std::uint32_t>(setLayouts.size());
  info.pSetLayouts = setLayouts.data();
  if (viewportPush) {
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &push;
  }
  VkPipelineLayout out = VK_NULL_HANDLE;
  vkCheck(vkCreatePipelineLayout(device, &info, nullptr, &out), "vkCreatePipelineLayout");
  return out;
}

VkPipeline VulkanPipelines::createPipeline(VkDevice device,
                                           SharedVulkanCore::Resources const& resources,
                                           VkPipelineLayout layout,
                                           unsigned char const* vertBytes,
                                           unsigned int vertLen,
                                           unsigned char const* fragBytes,
                                           unsigned int fragLen,
                                           VertexInput input,
                                           BlendMode blendMode,
                                           VkFormat colorFormatOverride) const {
  VkShaderModule vert = shaderModule(device, vertBytes, vertLen);
  VkShaderModule frag = shaderModule(device, fragBytes, fragLen);
  VkPipelineShaderStageCreateInfo stages[2]{
      vkStructure<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO),
      vkStructure<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO)};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";

  auto vi = vkStructure<VkPipelineVertexInputStateCreateInfo>(
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
  vi.vertexBindingDescriptionCount = input.bindingCount;
  vi.pVertexBindingDescriptions = input.bindings;
  vi.vertexAttributeDescriptionCount = input.attrCount;
  vi.pVertexAttributeDescriptions = input.attrs;
  auto ia =
      vkStructure<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  auto vp = vkStructure<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  auto rs =
      vkStructure<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.f;
  auto ms = vkStructure<VkPipelineMultisampleStateCreateInfo>(
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
  applyBlendModeToAttachment(blend, blendMode);
  auto cb = vkStructure<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
  cb.attachmentCount = 1;
  cb.pAttachments = &blend;
  VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  auto dyn = vkStructure<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = states;
  auto info = vkStructure<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
  auto rendering = vkStructure<VkPipelineRenderingCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
  VkFormat const colorFormat =
      colorFormatOverride == VK_FORMAT_UNDEFINED ? resources.renderFormat : colorFormatOverride;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &colorFormat;
  info.pNext = &rendering;
  info.stageCount = 2;
  info.pStages = stages;
  info.pVertexInputState = &vi;
  info.pInputAssemblyState = &ia;
  info.pViewportState = &vp;
  info.pRasterizationState = &rs;
  info.pMultisampleState = &ms;
  info.pColorBlendState = &cb;
  info.pDynamicState = &dyn;
  info.layout = layout;
  VkPipeline out = VK_NULL_HANDLE;
  vkCheck(vkCreateGraphicsPipelines(device, resources.pipelineCache, 1, &info, nullptr, &out),
          "vkCreateGraphicsPipelines");
  vkDestroyShaderModule(device, vert, nullptr);
  vkDestroyShaderModule(device, frag, nullptr);
  return out;
}

} // namespace lambdaui
