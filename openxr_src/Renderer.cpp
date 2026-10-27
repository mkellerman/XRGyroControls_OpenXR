#include "Renderer.h"

#include "Buffer.h"
#include "Context.h"
#include "Headset.h"
#include "Pipeline.h"
#include "RenderProcess.h"
#include "RenderTarget.h"
#include "Util.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>

#include <array>
#include <stdexcept>
#include "../openxr-Bridging-Header.h"

namespace
{
constexpr size_t numFramesInFlight = 2u;

VkFormat textureFormat(MTLTexture_id texture) {
  // MTLPixelFormat values from the selected SDK's Metal/MTLPixelFormat.h.
  // Importing BGRA as RGBA is invalid even though both use four bytes per pixel.
  switch (openxr_texture_pixel_format(texture)) {
    case 70: return VK_FORMAT_R8G8B8A8_UNORM;
    case 71: return VK_FORMAT_R8G8B8A8_SRGB;
    case 80: return VK_FORMAT_B8G8R8A8_UNORM;
    case 81: return VK_FORMAT_B8G8R8A8_SRGB;
    case 115: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: throw std::runtime_error("Unsupported Metal texture pixel format for Vulkan import");
  }
}

struct Vertex final
{
  glm::vec3 position;
  glm::vec3 color;
  glm::vec2 uv;
};

constexpr std::array vertices = {
  // framebuffer rect
  Vertex{{ -1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, {0.0f, 0.0f}}, Vertex{{ -1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, {0.0f, 1.0f}},
  Vertex{{ +1.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, {1.0f, 0.0f}}, Vertex{{ +1.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, {1.0f, 1.0f}},
};

constexpr std::array<uint16_t, 6u> indices = { 0u,  1u,  2u,  1u,  2u,  3u};
} // namespace

Renderer::Renderer(){}

uint32_t findMemoryType(const Context* context, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
  const VkPhysicalDevice vkPhysicalDevice = context->getVkPhysicalDevice();
  VkPhysicalDeviceMemoryProperties memProperties;
  vkGetPhysicalDeviceMemoryProperties(vkPhysicalDevice, &memProperties);

  for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
    if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
      return i;
    }
  }

  throw std::runtime_error("failed to find suitable memory type!");
}

void createBuffer(const Context* context, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
  const VkDevice vkDevice = context->getVkDevice();

  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = size;
  bufferInfo.usage = usage;
  bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  if (vkCreateBuffer(vkDevice, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
    throw std::runtime_error("failed to create buffer!");
  }

  VkMemoryRequirements memRequirements;
  vkGetBufferMemoryRequirements(vkDevice, buffer, &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = findMemoryType(context, memRequirements.memoryTypeBits, properties);

  if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate buffer memory!");
  }

  vkBindBufferMemory(vkDevice, buffer, bufferMemory, 0);
}

void createImage(const Context* context, uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& imageMemory) {
  const VkDevice vkDevice = context->getVkDevice();

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent.width = width;
  imageInfo.extent.height = height;
  imageInfo.extent.depth = 1;
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.format = format;
  imageInfo.tiling = tiling;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage = usage;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  if (vkCreateImage(vkDevice, &imageInfo, nullptr, &image) != VK_SUCCESS) {
    throw std::runtime_error("failed to create image!");
  }

  VkMemoryRequirements memRequirements;
  vkGetImageMemoryRequirements(vkDevice, image, &memRequirements);

  VkMemoryAllocateInfo allocInfo{};
  allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocInfo.allocationSize = memRequirements.size;
  allocInfo.memoryTypeIndex = findMemoryType(context, memRequirements.memoryTypeBits, properties);

  if (vkAllocateMemory(vkDevice, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
    throw std::runtime_error("failed to allocate image memory!");
  }

  vkBindImageMemory(vkDevice, image, imageMemory, 0);
}

void createImageFromMetal(const Context* context, uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage& image, MTLTexture_id tex_id) {
  const VkDevice vkDevice = context->getVkDevice();

  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.extent.width = width;
  imageInfo.extent.height = height;
  imageInfo.extent.depth = 1;
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 1;
  imageInfo.format = format;
  imageInfo.tiling = tiling;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageInfo.usage = usage;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkImportMetalTextureInfoEXT metalInfo{};
  metalInfo.sType = VK_STRUCTURE_TYPE_IMPORT_METAL_TEXTURE_INFO_EXT;
  metalInfo.pNext = nullptr;
  metalInfo.plane = VK_IMAGE_ASPECT_COLOR_BIT;
  metalInfo.mtlTexture = tex_id;

  imageInfo.pNext = &metalInfo;

  if (vkCreateImage(vkDevice, &imageInfo, nullptr, &image) != VK_SUCCESS) {
    throw std::runtime_error("failed to create image!");
  }
}

void Renderer::createTextureImageHax_L(const Context* context, int which) {
  const VkDevice vkDevice = context->getVkDevice();

  createImageFromMetal(context, metal_tex_w, metal_tex_h, textureFormat(metal_tex_l[which]), VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, textureImage_L[which], metal_tex_l[which]);
}

void Renderer::createTextureImageHax_R(const Context* context, int which) {
  const VkDevice vkDevice = context->getVkDevice();

  createImageFromMetal(context, metal_tex_w, metal_tex_h, textureFormat(metal_tex_r[which]), VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, textureImage_R[which], metal_tex_r[which]);
}

Renderer::Renderer(const Context* context, const Headset* headset, MTLTexture_id* tex_l, MTLTexture_id* tex_r, uint32_t tex_w, uint32_t tex_h) : context(context), headset(headset)
{
  const VkPhysicalDevice vkPhysicalDevice = context->getVkPhysicalDevice();
  const VkDevice vkDevice = context->getVkDevice();
  for (int i = 0; i < 3; i++)
  {
    metal_tex_l[i] = tex_l[i];
    metal_tex_r[i] = tex_r[i];
  }
  metal_tex_w = tex_w;
  metal_tex_h = tex_h;

  // Create a command pool
  VkCommandPoolCreateInfo commandPoolCreateInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
  commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  commandPoolCreateInfo.queueFamilyIndex = context->getVkDrawQueueFamilyIndex();
  if (vkCreateCommandPool(vkDevice, &commandPoolCreateInfo, nullptr, &commandPool) != VK_SUCCESS)
  {
    util::error(Error::GenericVulkan);
    valid = false;
    return;
  }

  for (int i = 0; i < 3; i++)
  {
    createTextureImageHax_L(context,i);

    createTextureImageHax_R(context,i);
  }

  // Create a render process for each frame in flight
  int which = 0;
  renderProcesses.resize(numFramesInFlight);
  for (RenderProcess*& renderProcess : renderProcesses)
  {
    renderProcess = new RenderProcess(vkDevice, vkPhysicalDevice, commandPool);
    if (!renderProcess->isValid())
    {
      valid = false;
      return;
    }
  }
}

Renderer::~Renderer()
{
  const VkDevice vkDevice = context->getVkDevice();

  for (int i = 0; i < 3; i++)
  {
    vkDestroyImage(vkDevice, textureImage_L[i], nullptr);
    vkDestroyImage(vkDevice, textureImage_R[i], nullptr);
  }

  for (const RenderProcess* renderProcess : renderProcesses)
  {
    delete renderProcess;
  }

  vkDestroyCommandPool(vkDevice, commandPool, nullptr);
}

void Renderer::render(size_t swapchainImageIndex, int which)
{
  currentRenderProcessIndex = (currentRenderProcessIndex + 1u) % renderProcesses.size();

  //which = 0;
  RenderProcess* renderProcess = renderProcesses.at(currentRenderProcessIndex);

  const VkFence busyFence = renderProcess->getBusyFence();
  if (vkResetFences(context->getVkDevice(), 1u, &busyFence) != VK_SUCCESS)
  {
    return;
  }

  const VkCommandBuffer commandBuffer = renderProcess->getCommandBuffer();

  if (vkResetCommandBuffer(commandBuffer, 0u) != VK_SUCCESS)
  {
    return;
  }

  VkCommandBufferBeginInfo commandBufferBeginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
  if (vkBeginCommandBuffer(commandBuffer, &commandBufferBeginInfo) != VK_SUCCESS)
  {
    return;
  }

  // Do a 32x32 blit to all of the dst image - should get big squares
  VkImageBlit region_l;
  region_l.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region_l.srcSubresource.mipLevel = 0;
  region_l.srcSubresource.baseArrayLayer = 0;
  region_l.srcSubresource.layerCount = 1;
  region_l.srcOffsets[0].x = 0;
  region_l.srcOffsets[0].y = 0;
  region_l.srcOffsets[0].z = 0;
  region_l.srcOffsets[1].x = metal_tex_w;
  region_l.srcOffsets[1].y = metal_tex_h;
  region_l.srcOffsets[1].z = 1;
  region_l.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region_l.dstSubresource.mipLevel = 0;
  region_l.dstSubresource.baseArrayLayer = 0;
  region_l.dstSubresource.layerCount = 1;
  region_l.dstOffsets[0].x = 0;
  region_l.dstOffsets[0].y = 0;
  region_l.dstOffsets[0].z = 0;
  region_l.dstOffsets[1].x = headset->getRenderTarget(swapchainImageIndex)->w;
  region_l.dstOffsets[1].y = headset->getRenderTarget(swapchainImageIndex)->h;
  region_l.dstOffsets[1].z = 1;

  VkImageBlit region_r;
  region_r.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region_r.srcSubresource.mipLevel = 0;
  region_r.srcSubresource.baseArrayLayer = 0;
  region_r.srcSubresource.layerCount = 1;
  region_r.srcOffsets[0].x = 0;
  region_r.srcOffsets[0].y = 0;
  region_r.srcOffsets[0].z = 0;
  region_r.srcOffsets[1].x = metal_tex_w;
  region_r.srcOffsets[1].y = metal_tex_h;
  region_r.srcOffsets[1].z = 1;
  region_r.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region_r.dstSubresource.mipLevel = 0;
  region_r.dstSubresource.baseArrayLayer = 1;
  region_r.dstSubresource.layerCount = 1;
  region_r.dstOffsets[0].x = 0;
  region_r.dstOffsets[0].y = 0;
  region_r.dstOffsets[0].z = 0;
  region_r.dstOffsets[1].x = headset->getRenderTarget(swapchainImageIndex)->w;
  region_r.dstOffsets[1].y = headset->getRenderTarget(swapchainImageIndex)->h;
  region_r.dstOffsets[1].z = 1;

  vkCmdBlitImage(commandBuffer, textureImage_L[which], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, headset->getRenderTarget(swapchainImageIndex)->getImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region_l, VK_FILTER_LINEAR);
  vkCmdBlitImage(commandBuffer, textureImage_R[which], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, headset->getRenderTarget(swapchainImageIndex)->getImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &region_r, VK_FILTER_LINEAR);
}

void Renderer::submit(bool useSemaphores, int which) const
{
  const RenderProcess* renderProcess = renderProcesses.at(currentRenderProcessIndex);
  const VkCommandBuffer commandBuffer = renderProcess->getCommandBuffer();
  if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS)
  {
    return;
  }

  const VkPipelineStageFlags waitStages = { 0xFFFFFFFF };
  const VkSemaphore drawableSemaphore = renderProcess->getDrawableSemaphore();
  const VkSemaphore presentableSemaphore = renderProcess->getPresentableSemaphore();
  const VkFence busyFence = renderProcess->getBusyFence();

  VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
  submitInfo.pWaitDstStageMask = &waitStages;
  submitInfo.commandBufferCount = 1u;
  submitInfo.pCommandBuffers = &commandBuffer;

  if (useSemaphores)
  {
    submitInfo.waitSemaphoreCount = 1u;
    submitInfo.pWaitSemaphores = &drawableSemaphore;
    submitInfo.signalSemaphoreCount = 1u;
    submitInfo.pSignalSemaphores = &presentableSemaphore;
  }

  if (vkQueueSubmit(context->getVkDrawQueue(), 1u, &submitInfo, busyFence) != VK_SUCCESS)
  {
    return;
  }

  vkQueueWaitIdle(context->getVkDrawQueue());
}

bool Renderer::isValid() const
{
  return valid;
}

VkCommandBuffer Renderer::getCurrentCommandBuffer() const
{
  return renderProcesses.at(currentRenderProcessIndex)->getCommandBuffer();
}

VkSemaphore Renderer::getCurrentDrawableSemaphore() const
{
  return renderProcesses.at(currentRenderProcessIndex)->getDrawableSemaphore();
}

VkSemaphore Renderer::getCurrentPresentableSemaphore() const
{
  return renderProcesses.at(currentRenderProcessIndex)->getPresentableSemaphore();
}
