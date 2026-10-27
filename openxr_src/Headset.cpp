#include "Headset.h"
#include "../openxr-Bridging-Header.h"

#include "Context.h"
#include "RenderTarget.h"
#include "Util.h"

#include <array>
#include <sstream>
#include <dlfcn.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>

namespace
{
constexpr XrReferenceSpaceType spaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
constexpr VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
} // namespace

static XrPosef identity_pose = {.orientation = {.x = 0, .y = 0, .z = 0, .w = 1.0},
                                .position = {.x = 0, .y = 0, .z = 0}};

// Quest 3 has no eye tracker. Older Monado builds also omit these optional
// Quest Pro sidechannel exports; neither case may prevent the bridge loading.
static glm::quat eye_orientation(const char* symbol) {
  const float* values = static_cast<const float*>(dlsym(RTLD_DEFAULT, symbol));
  if (!values) return glm::quat(1, 0, 0, 0);
  glm::quat result(values[3], values[0], values[1], values[2]);
  float length = glm::length(result);
  if (!std::isfinite(length) || length < 0.001f) return glm::quat(1, 0, 0, 0);
  return glm::normalize(result);
}

Headset::Headset(){}

Headset::Headset(const Context* context) : context(context)
{
  const VkDevice device = context->getVkDevice();

  lastPoseIdx = 0;

  // Create a render pass
  {
    constexpr uint32_t viewMask = 0b00000011;
    constexpr uint32_t correlationMask = 0b00000011;

    VkRenderPassMultiviewCreateInfo renderPassMultiviewCreateInfo{
      VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO
    };
    renderPassMultiviewCreateInfo.subpassCount = 1u;
    renderPassMultiviewCreateInfo.pViewMasks = &viewMask;
    renderPassMultiviewCreateInfo.correlationMaskCount = 1u;
    renderPassMultiviewCreateInfo.pCorrelationMasks = &correlationMask;

    VkAttachmentDescription colorAttachmentDescription{};
    colorAttachmentDescription.format = colorFormat;
    colorAttachmentDescription.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachmentDescription.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachmentDescription.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachmentDescription.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachmentDescription.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorAttachmentReference;
    colorAttachmentReference.attachment = 0u;
    colorAttachmentReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depthAttachmentDescription{};
    depthAttachmentDescription.format = depthFormat;
    depthAttachmentDescription.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachmentDescription.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachmentDescription.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachmentDescription.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachmentDescription.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentReference;
    depthAttachmentReference.attachment = 1u;
    depthAttachmentReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpassDescription{};
    subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpassDescription.colorAttachmentCount = 1u;
    subpassDescription.pColorAttachments = &colorAttachmentReference;
    subpassDescription.pDepthStencilAttachment = &depthAttachmentReference;

    const std::array attachments = { colorAttachmentDescription, depthAttachmentDescription };

    VkRenderPassCreateInfo renderPassCreateInfo{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    renderPassCreateInfo.pNext = &renderPassMultiviewCreateInfo;
    renderPassCreateInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassCreateInfo.pAttachments = attachments.data();
    renderPassCreateInfo.subpassCount = 1u;
    renderPassCreateInfo.pSubpasses = &subpassDescription;
    if (vkCreateRenderPass(device, &renderPassCreateInfo, nullptr, &renderPass) != VK_SUCCESS)
    {
      util::error(Error::GenericVulkan);
      valid = false;
      return;
    }
  }

  const XrInstance xrInstance = context->getXrInstance();
  const XrSystemId xrSystemId = context->getXrSystemId();
  const VkPhysicalDevice vkPhysicalDevice = context->getVkPhysicalDevice();
  const uint32_t vkDrawQueueFamilyIndex = context->getVkDrawQueueFamilyIndex();

  // Create a session with Vulkan graphics binding
  XrGraphicsBindingVulkanKHR graphicsBinding{ XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR };
  graphicsBinding.device = device;
  graphicsBinding.instance = context->getVkInstance();
  graphicsBinding.physicalDevice = vkPhysicalDevice;
  graphicsBinding.queueFamilyIndex = vkDrawQueueFamilyIndex;
  graphicsBinding.queueIndex = 0u;

  XrSessionCreateInfo sessionCreateInfo{ XR_TYPE_SESSION_CREATE_INFO };
  sessionCreateInfo.next = &graphicsBinding;
  sessionCreateInfo.systemId = xrSystemId;
  XrResult result = xrCreateSession(xrInstance, &sessionCreateInfo, &session);
  if (XR_FAILED(result))
  {
    util::error(Error::GenericOpenXR);
    valid = false;
    return;
  }

  // Create a play space
  XrReferenceSpaceCreateInfo referenceSpaceCreateInfo{ XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
  referenceSpaceCreateInfo.referenceSpaceType = spaceType;
  referenceSpaceCreateInfo.poseInReferenceSpace = util::makeIdentity();
  result = xrCreateReferenceSpace(session, &referenceSpaceCreateInfo, &space);
  if (XR_FAILED(result))
  {
    util::error(Error::GenericOpenXR);
    valid = false;
    return;
  }

  const XrViewConfigurationType viewType = context->getXrViewType();

  // Get the number of eyes
  result = xrEnumerateViewConfigurationViews(xrInstance, xrSystemId, viewType, 0u,
                                             reinterpret_cast<uint32_t*>(&eyeCount), nullptr);
  if (XR_FAILED(result))
  {
    util::error(Error::GenericOpenXR);
    valid = false;
    return;
  }

  // Get the eye image info per eye
  eyeImageInfos.resize(eyeCount);
  for (XrViewConfigurationView& eyeInfo : eyeImageInfos)
  {
    eyeInfo.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    eyeInfo.next = nullptr;
  }

  result =
    xrEnumerateViewConfigurationViews(xrInstance, xrSystemId, viewType, static_cast<uint32_t>(eyeImageInfos.size()),
                                      reinterpret_cast<uint32_t*>(&eyeCount), eyeImageInfos.data());
  if (XR_FAILED(result))
  {
    util::error(Error::GenericOpenXR);
    valid = false;
    return;
  }

  // Verify that the desired color format is supported
  {
    uint32_t formatCount = 0u;
    result = xrEnumerateSwapchainFormats(session, 0u, &formatCount, nullptr);
    if (XR_FAILED(result))
    {
      util::error(Error::GenericOpenXR);
      valid = false;
      return;
    }

    std::vector<int64_t> formats(formatCount);
    result = xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data());
    if (XR_FAILED(result))
    {
      util::error(Error::GenericOpenXR);
      valid = false;
      return;
    }

    bool formatFound = false;
    for (const int64_t& format : formats)
    {
      if (format == static_cast<int64_t>(colorFormat))
      {
        formatFound = true;
        break;
      }
    }

    if (!formatFound)
    {
      util::error(Error::FeatureNotSupported, "OpenXR swapchain color format");
      valid = false;
      return;
    }
  }

  const VkExtent2D eyeResolution = getEyeResolution(0u);

  // Create a depth buffer
  {
    // Create an image
    VkImageCreateInfo imageCreateInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.extent.width = eyeResolution.width;
    imageCreateInfo.extent.height = eyeResolution.height;
    imageCreateInfo.extent.depth = 1u;
    imageCreateInfo.mipLevels = 1u;
    imageCreateInfo.arrayLayers = 2u;
    imageCreateInfo.format = depthFormat;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCreateInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device, &imageCreateInfo, nullptr, &depthImage) != VK_SUCCESS)
    {
      util::error(Error::GenericVulkan);
      valid = false;
      return;
    }

    VkMemoryRequirements memoryRequirements;
    vkGetImageMemoryRequirements(device, depthImage, &memoryRequirements);

    VkPhysicalDeviceMemoryProperties supportedMemoryProperties;
    vkGetPhysicalDeviceMemoryProperties(vkPhysicalDevice, &supportedMemoryProperties);

    const VkMemoryPropertyFlags memoryProperties = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    const VkMemoryPropertyFlags typeFilter = memoryRequirements.memoryTypeBits;
    uint32_t memoryTypeIndex = 0u;
    bool memoryTypeFound = false;
    for (uint32_t i = 0u; i < supportedMemoryProperties.memoryTypeCount; ++i)
    {
      const VkMemoryPropertyFlags propertyFlags = supportedMemoryProperties.memoryTypes[i].propertyFlags;
      if (typeFilter & (1 << i) && (propertyFlags & memoryProperties) == memoryProperties)
      {
        memoryTypeIndex = i;
        memoryTypeFound = true;
        break;
      }
    }

    if (!memoryTypeFound)
    {
      util::error(Error::FeatureNotSupported, "Suitable depth buffer memory type");
      valid = false;
      return;
    }

    VkMemoryAllocateInfo memoryAllocateInfo{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    memoryAllocateInfo.allocationSize = memoryRequirements.size;
    memoryAllocateInfo.memoryTypeIndex = memoryTypeIndex;
    if (vkAllocateMemory(device, &memoryAllocateInfo, nullptr, &depthMemory) != VK_SUCCESS)
    {
      std::stringstream s;
      s << memoryRequirements.size << " bytes for depth buffer";
      util::error(Error::OutOfMemory, s.str());
      valid = false;
      return;
    }

    if (vkBindImageMemory(device, depthImage, depthMemory, 0) != VK_SUCCESS)
    {
      util::error(Error::GenericVulkan);
      valid = false;
      return;
    }

    // Create an image view
    VkImageViewCreateInfo imageViewCreateInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    imageViewCreateInfo.image = depthImage;
    imageViewCreateInfo.format = depthFormat;
    imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    imageViewCreateInfo.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    imageViewCreateInfo.subresourceRange.layerCount = 2u;
    imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    imageViewCreateInfo.subresourceRange.baseArrayLayer = 0u;
    imageViewCreateInfo.subresourceRange.baseMipLevel = 0u;
    imageViewCreateInfo.subresourceRange.levelCount = 1u;
    if (vkCreateImageView(device, &imageViewCreateInfo, nullptr, &depthImageView) != VK_SUCCESS)
    {
      util::error(Error::GenericVulkan);
      valid = false;
      return;
    }
  }

  // Create a swapchain and render targets
  {
    const XrViewConfigurationView& eyeImageInfo = eyeImageInfos.at(0u);

    // Create a swapchain
    XrSwapchainCreateInfo swapchainCreateInfo{ XR_TYPE_SWAPCHAIN_CREATE_INFO };
    swapchainCreateInfo.format = colorFormat;
    swapchainCreateInfo.sampleCount = eyeImageInfo.recommendedSwapchainSampleCount;
    swapchainCreateInfo.width = eyeImageInfo.recommendedImageRectWidth;
    swapchainCreateInfo.height = eyeImageInfo.recommendedImageRectHeight;
    swapchainCreateInfo.arraySize = static_cast<uint32_t>(eyeCount);
    swapchainCreateInfo.faceCount = 1u;
    swapchainCreateInfo.mipCount = 1u;

    result = xrCreateSwapchain(session, &swapchainCreateInfo, &swapchain);
    if (XR_FAILED(result))
    {
      util::error(Error::GenericOpenXR);
      valid = false;
      return;
    }

    // Get the number of swapchain images
    uint32_t swapchainImageCount;
    result = xrEnumerateSwapchainImages(swapchain, 0u, &swapchainImageCount, nullptr);
    if (XR_FAILED(result))
    {
      util::error(Error::GenericOpenXR);
      valid = false;
      return;
    }

    // Retrieve the swapchain images
    std::vector<XrSwapchainImageVulkanKHR> swapchainImages;
    swapchainImages.resize(swapchainImageCount);
    for (XrSwapchainImageVulkanKHR& swapchainImage : swapchainImages)
    {
      swapchainImage.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    }

    XrSwapchainImageBaseHeader* data = reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchainImages.data());
    result =
      xrEnumerateSwapchainImages(swapchain, static_cast<uint32_t>(swapchainImages.size()), &swapchainImageCount, data);
    if (XR_FAILED(result))
    {
      util::error(Error::GenericOpenXR);
      valid = false;
      return;
    }

    // Create the render targets
    swapchainRenderTargets.resize(swapchainImages.size());
    for (size_t renderTargetIndex = 0u; renderTargetIndex < swapchainRenderTargets.size(); ++renderTargetIndex)
    {
      RenderTarget*& renderTarget = swapchainRenderTargets.at(renderTargetIndex);

      const VkImage image = swapchainImages.at(renderTargetIndex).image;
      renderTarget = new RenderTarget(device, image, depthImageView, eyeResolution, colorFormat, renderPass, 2u);
      if (!renderTarget->isValid())
      {
        valid = false;
        return;
      }
    }
  }

  // Actions
  // --- Set up input (actions)

  xrStringToPath(xrInstance, "/user/hand/left", &hand_paths[HAND_LEFT_INDEX]);
  xrStringToPath(xrInstance, "/user/hand/right", &hand_paths[HAND_RIGHT_INDEX]);

  
  xrStringToPath(xrInstance, "/user/hand/left/input/select/click",
                 &select_click_path[HAND_LEFT_INDEX]);
  xrStringToPath(xrInstance, "/user/hand/right/input/select/click",
                 &select_click_path[HAND_RIGHT_INDEX]);

  xrStringToPath(xrInstance, "/user/hand/left/input/system/click",
                 &system_click_path[HAND_LEFT_INDEX]);
  xrStringToPath(xrInstance, "/user/hand/right/input/system/click",
                 &system_click_path[HAND_RIGHT_INDEX]);

  xrStringToPath(xrInstance, "/user/hand/left/input/menu/click",
                 &menu_click_path[HAND_LEFT_INDEX]);
  xrStringToPath(xrInstance, "/user/hand/right/input/menu/click",
                 &menu_click_path[HAND_RIGHT_INDEX]);

  xrStringToPath(xrInstance, "/user/hand/left/input/b/click",
                 &b_click_path[HAND_LEFT_INDEX]);
  xrStringToPath(xrInstance, "/user/hand/right/input/b/click",
                 &b_click_path[HAND_RIGHT_INDEX]);

  xrStringToPath(xrInstance, "/user/hand/left/input/y/click",
                 &y_click_path[HAND_LEFT_INDEX]);
  xrStringToPath(xrInstance, "/user/hand/right/input/y/click",
                 &y_click_path[HAND_RIGHT_INDEX]);

  
  xrStringToPath(xrInstance, "/user/hand/left/input/trigger/value",
                 &trigger_value_path[HAND_LEFT_INDEX]);
  xrStringToPath(xrInstance, "/user/hand/right/input/trigger/value",
                 &trigger_value_path[HAND_RIGHT_INDEX]);

  xrStringToPath(xrInstance, "/user/hand/left/input/squeeze/value",
                 &grip_value_path[HAND_LEFT_INDEX]);
  xrStringToPath(xrInstance, "/user/hand/right/input/squeeze/value",
                 &grip_value_path[HAND_RIGHT_INDEX]);
  
  xrStringToPath(xrInstance, "/user/hand/left/input/thumbstick/y",
                 &thumbstick_y_path[HAND_LEFT_INDEX]);
  xrStringToPath(xrInstance, "/user/hand/right/input/thumbstick/y",
                 &thumbstick_y_path[HAND_RIGHT_INDEX]);
  
  xrStringToPath(xrInstance, "/user/hand/left/input/grip/pose", &grip_pose_path[HAND_LEFT_INDEX]);
  xrStringToPath(xrInstance, "/user/hand/right/input/grip/pose", &grip_pose_path[HAND_RIGHT_INDEX]);

  XrActionSetCreateInfo gameplay_actionset_info = {
      .type = XR_TYPE_ACTION_SET_CREATE_INFO, .next = NULL, .priority = 0};
  strcpy(gameplay_actionset_info.actionSetName, "gameplay_actionset");
  strcpy(gameplay_actionset_info.localizedActionSetName, "Gameplay Actions");

  result = xrCreateActionSet(xrInstance, &gameplay_actionset_info, &gameplay_actionset);
  if (XR_SUCCEEDED(result))

  {
    XrActionCreateInfo action_info = {.type = XR_TYPE_ACTION_CREATE_INFO,
                                      .next = NULL,
                                      .actionType = XR_ACTION_TYPE_POSE_INPUT,
                                      .countSubactionPaths = HAND_COUNT,
                                      .subactionPaths = hand_paths};
    strcpy(action_info.actionName, "handpose");
    strcpy(action_info.localizedActionName, "Hand Pose");

    result = xrCreateAction(gameplay_actionset, &action_info, &hand_pose_action);
    //if (!XR_SUCCEEDED(result))
    //  return 1;
  }
  // poses can't be queried directly, we need to create a space for each
  
  for (int hand = 0; hand < HAND_COUNT; hand++) {
    XrActionSpaceCreateInfo action_space_info = {.type = XR_TYPE_ACTION_SPACE_CREATE_INFO,
                                                 .next = NULL,
                                                 .action = hand_pose_action,
                                                 .poseInActionSpace = identity_pose,
                                                 .subactionPath = hand_paths[hand]};

    result = xrCreateActionSpace(session, &action_space_info, &hand_pose_spaces[hand]);
    //if (!XR_SUCCEEDED(result))
    //  return 1;
  }

  // Grabbing objects is not actually implemented in this demo, it only gives some  haptic feebdack.
  
  {
    XrActionCreateInfo action_info = {.type = XR_TYPE_ACTION_CREATE_INFO,
                                      .next = NULL,
                                      .actionType = XR_ACTION_TYPE_FLOAT_INPUT,
                                      .countSubactionPaths = HAND_COUNT,
                                      .subactionPaths = hand_paths};
    strcpy(action_info.actionName, "grabobjectfloat");
    strcpy(action_info.localizedActionName, "Grab Object");

    result = xrCreateAction(gameplay_actionset, &action_info, &grab_action_float);
    //if (!XR_SUCCEEDED(result))
    //  return 1;
  }

  {
    XrActionCreateInfo action_info = {.type = XR_TYPE_ACTION_CREATE_INFO,
                                      .next = NULL,
                                      .actionType = XR_ACTION_TYPE_FLOAT_INPUT,
                                      .countSubactionPaths = HAND_COUNT,
                                      .subactionPaths = hand_paths};
    strcpy(action_info.actionName, "gripobjectfloat");
    strcpy(action_info.localizedActionName, "Grip Object");

    result = xrCreateAction(gameplay_actionset, &action_info, &grip_action_float);
    //if (!XR_SUCCEEDED(result))
    //  return 1;
  }

  {
    XrActionCreateInfo action_info = {.type = XR_TYPE_ACTION_CREATE_INFO,
                                      .next = NULL,
                                      .actionType = XR_ACTION_TYPE_BOOLEAN_INPUT,
                                      .countSubactionPaths = HAND_COUNT,
                                      .subactionPaths = hand_paths};
    strcpy(action_info.actionName, "systemactionbool");
    strcpy(action_info.localizedActionName, "Home Button");

    result = xrCreateAction(gameplay_actionset, &action_info, &system_action_bool);
    //if (!XR_SUCCEEDED(result))
    //  return 1;
  }

  {
    XrActionCreateInfo action_info = {.type = XR_TYPE_ACTION_CREATE_INFO,
                                      .next = NULL,
                                      .actionType = XR_ACTION_TYPE_BOOLEAN_INPUT,
                                      .countSubactionPaths = HAND_COUNT,
                                      .subactionPaths = hand_paths};
    strcpy(action_info.actionName, "byactionbool");
    strcpy(action_info.localizedActionName, "Touching Button");

    result = xrCreateAction(gameplay_actionset, &action_info, &b_y_action_bool);
    //if (!XR_SUCCEEDED(result))
    //  return 1;
  }

  // suggest actions for valve index controller
  {
    XrPath interaction_profile_path;
    result = xrStringToPath(xrInstance, "/interaction_profiles/valve/index_controller",
                            &interaction_profile_path);
    //if (!xr_check(xrInstance, result, "failed to get interaction profile"))
    //  return 1;

    const XrActionSuggestedBinding bindings[] = {
        {.action = hand_pose_action, .binding = grip_pose_path[HAND_LEFT_INDEX]},
        {.action = hand_pose_action, .binding = grip_pose_path[HAND_RIGHT_INDEX]},
        {.action = grab_action_float, .binding = trigger_value_path[HAND_LEFT_INDEX]},
        {.action = grab_action_float, .binding = trigger_value_path[HAND_RIGHT_INDEX]},
        {.action = grip_action_float, .binding = grip_value_path[HAND_LEFT_INDEX]},
        {.action = grip_action_float, .binding = grip_value_path[HAND_RIGHT_INDEX]},
        {.action = system_action_bool, .binding = system_click_path[HAND_LEFT_INDEX]},
        {.action = system_action_bool, .binding = system_click_path[HAND_RIGHT_INDEX]},
        {.action = b_y_action_bool, .binding = b_click_path[HAND_LEFT_INDEX]},
        {.action = b_y_action_bool, .binding = b_click_path[HAND_RIGHT_INDEX]},
    };

    const XrInteractionProfileSuggestedBinding suggested_bindings = {
        .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
        .next = NULL,
        .interactionProfile = interaction_profile_path,
        .countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]),
        .suggestedBindings = bindings};

    xrSuggestInteractionProfileBindings(xrInstance, &suggested_bindings);
    //if (!xr_check(xrInstance, result, "failed to suggest bindings"))
    //  return 1;
  }

  // suggest actions for Oculus Touch-type controllers
  {
    XrPath interaction_profile_path;
    result = xrStringToPath(xrInstance, "/interaction_profiles/oculus/touch_controller",
                            &interaction_profile_path);
    //if (!xr_check(xrInstance, result, "failed to get interaction profile"))
    //  return 1;

    const XrActionSuggestedBinding bindings[] = {
        {.action = hand_pose_action, .binding = grip_pose_path[HAND_LEFT_INDEX]},
        {.action = hand_pose_action, .binding = grip_pose_path[HAND_RIGHT_INDEX]},
        {.action = grab_action_float, .binding = trigger_value_path[HAND_LEFT_INDEX]},
        {.action = grab_action_float, .binding = trigger_value_path[HAND_RIGHT_INDEX]},
        {.action = grip_action_float, .binding = grip_value_path[HAND_LEFT_INDEX]},
        {.action = grip_action_float, .binding = grip_value_path[HAND_RIGHT_INDEX]},
        {.action = system_action_bool, .binding = menu_click_path[HAND_LEFT_INDEX]},
        {.action = system_action_bool, .binding = system_click_path[HAND_RIGHT_INDEX]},
        {.action = b_y_action_bool, .binding = y_click_path[HAND_LEFT_INDEX]},
        {.action = b_y_action_bool, .binding = b_click_path[HAND_RIGHT_INDEX]},
    };

    const XrInteractionProfileSuggestedBinding suggested_bindings = {
        .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
        .next = NULL,
        .interactionProfile = interaction_profile_path,
        .countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]),
        .suggestedBindings = bindings};

    xrSuggestInteractionProfileBindings(xrInstance, &suggested_bindings);
    //if (!xr_check(xrInstance, result, "failed to suggest bindings"))
    //  return 1;
  }


#if 0
  // suggest actions for simple controller
  {
    XrPath interaction_profile_path;
    result = xrStringToPath(xrInstance, "/interaction_profiles/khr/simple_controller",
                            &interaction_profile_path);
    //if (!xr_check(xrInstance, result, "failed to get interaction profile"))
    //  return 1;

    const XrActionSuggestedBinding bindings[] = {
        {.action = hand_pose_action, .binding = grip_pose_path[HAND_LEFT_INDEX]},
        {.action = hand_pose_action, .binding = grip_pose_path[HAND_RIGHT_INDEX]},
        // boolean input select/click will be converted to float that is either 0 or 1
        {.action = grab_action_float, .binding = select_click_path[HAND_LEFT_INDEX]},
        {.action = grab_action_float, .binding = select_click_path[HAND_RIGHT_INDEX]},
        {.action = system_action_bool, .binding = select_click_path[HAND_LEFT_INDEX]},
        {.action = system_action_bool, .binding = select_click_path[HAND_RIGHT_INDEX]},
    };

    const XrInteractionProfileSuggestedBinding suggested_bindings = {
        .type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
        .next = NULL,
        .interactionProfile = interaction_profile_path,
        .countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]),
        .suggestedBindings = bindings};

    xrSuggestInteractionProfileBindings(xrInstance, &suggested_bindings);
    //if (!xr_check(xrInstance, result, "failed to suggest bindings"))
    //  return 1;
  }
#endif

  XrSessionActionSetsAttachInfo actionset_attach_info = {
      .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,
      .next = NULL,
      .countActionSets = 1,
      .actionSets = &gameplay_actionset};
  result = xrAttachSessionActionSets(session, &actionset_attach_info);
  //if (!xr_check(instance, result, "failed to attach action set"))
  //  return 1;

  // Create a hand tracker for left hand that tracks default set of hand joints.
  {
      XrHandTrackerCreateInfoEXT createInfo{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
      createInfo.hand = XR_HAND_LEFT_EXT;
      createInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
      left_hand_valid = XR_SUCCEEDED(context->xrCreateHandTrackerEXT(session, &createInfo, &leftHandTracker));
  }

  // right hand
  {
      XrHandTrackerCreateInfoEXT createInfo{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
      createInfo.hand = XR_HAND_RIGHT_EXT;
      createInfo.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
      right_hand_valid = XR_SUCCEEDED(context->xrCreateHandTrackerEXT(session, &createInfo, &rightHandTracker));
  }

  // Allocate buffers to receive joint location and velocity data before frame
  // loop starts

  for (int i = 0; i < STORED_POSE_COUNT; i++)
  {
    PoseData* p = &storedPoses[i];

    // Create the eye render infos
    p->eyeRenderInfos.resize(eyeCount);
    for (size_t eyeIndex = 0u; eyeIndex < p->eyeRenderInfos.size(); ++eyeIndex)
    {
      XrCompositionLayerProjectionView& eyeRenderInfo = p->eyeRenderInfos.at(eyeIndex);
      eyeRenderInfo.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
      eyeRenderInfo.next = nullptr;

      // Associate this eye with the swapchain
      const XrViewConfigurationView& eyeImageInfo = eyeImageInfos.at(eyeIndex);
      eyeRenderInfo.subImage.swapchain = swapchain;
      eyeRenderInfo.subImage.imageArrayIndex = static_cast<uint32_t>(eyeIndex);
      eyeRenderInfo.subImage.imageRect.offset = { 0, 0 };
      eyeRenderInfo.subImage.imageRect.extent = { static_cast<int32_t>(eyeImageInfo.recommendedImageRectWidth),
                                                  static_cast<int32_t>(eyeImageInfo.recommendedImageRectHeight) };
    }

    // Allocate the eye poses
    p->eyePoses.resize(eyeCount);
    for (XrView& eyePose : p->eyePoses)
    {
      eyePose.type = XR_TYPE_VIEW;
      eyePose.next = nullptr;
    }

    // Allocate view and projection matrices
    p->eyeViewMatrices.resize(eyeCount);
    p->eyeProjectionMatrices.resize(eyeCount);

    p->leftVelocities = {XR_TYPE_HAND_JOINT_VELOCITIES_EXT};
    p->leftVelocities.jointCount = XR_HAND_JOINT_COUNT_EXT;
    p->leftVelocities.jointVelocities = p->leftJointVelocities;

    p->leftLocations = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
    p->leftLocations.next = &p->leftVelocities;
    p->leftLocations.jointCount = XR_HAND_JOINT_COUNT_EXT;
    p->leftLocations.jointLocations = p->leftJointLocations;

    p->rightVelocities = {XR_TYPE_HAND_JOINT_VELOCITIES_EXT};
    p->rightVelocities.jointCount = XR_HAND_JOINT_COUNT_EXT;
    p->rightVelocities.jointVelocities = p->rightJointVelocities;

    p->rightLocations = {XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
    p->rightLocations.next = &p->rightVelocities;
    p->rightLocations.jointCount = XR_HAND_JOINT_COUNT_EXT;
    p->rightLocations.jointLocations = p->rightJointLocations;
  }
}

Headset::~Headset()
{
  // Clean up OpenXR
  xrEndSession(session);
  xrDestroySwapchain(swapchain);

  for (const RenderTarget* renderTarget : swapchainRenderTargets)
  {
    delete renderTarget;
  }

  xrDestroySpace(space);
  xrDestroySession(session);

  // Clean up Vulkan
  const VkDevice vkDevice = context->getVkDevice();
  vkDestroyImageView(vkDevice, depthImageView, nullptr);
  vkFreeMemory(vkDevice, depthMemory, nullptr);
  vkDestroyImage(vkDevice, depthImage, nullptr);
  vkDestroyRenderPass(vkDevice, renderPass, nullptr);
}

void Headset::redoBeginFrame()
{
  // Begin the new frame
  XrFrameBeginInfo frameBeginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
  XrResult result = xrBeginFrame(session, &frameBeginInfo);
  if (XR_FAILED(result))
  {
    util::error(Error::GenericOpenXR);
    return;// BeginFrameResult::Error;
  }
  
}

Headset::BeginFrameResult Headset::beginFrame(int* pPoseIdx)
{
  const XrInstance instance = context->getXrInstance();
  int curPoseIdx = (lastPoseIdx + 1) % STORED_POSE_COUNT;
  lastPoseIdx = curPoseIdx;
  PoseData* pOut = &storedPoses[curPoseIdx];

  if (pPoseIdx) {
    *pPoseIdx = curPoseIdx;
  }

  // Poll OpenXR events
  XrEventDataBuffer buffer;
  buffer.type = XR_TYPE_EVENT_DATA_BUFFER;
  while (xrPollEvent(instance, &buffer) == XR_SUCCESS)
  {
    switch (buffer.type)
    {
    case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
      exitRequested = true;
      return BeginFrameResult::SkipFully;
    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
    {
      XrEventDataSessionStateChanged* event = reinterpret_cast<XrEventDataSessionStateChanged*>(&buffer);
      sessionState = event->state;

      if (event->state == XR_SESSION_STATE_READY)
      {
        if (!beginSession())
        {
          return BeginFrameResult::Error;
        }
      }
      else if (event->state == XR_SESSION_STATE_STOPPING)
      {
        if (!endSession())
        {
          return BeginFrameResult::Error;
        }
      }
      else if (event->state == XR_SESSION_STATE_LOSS_PENDING || event->state == XR_SESSION_STATE_EXITING)
      {
        exitRequested = true;
        return BeginFrameResult::SkipFully;
      }

      break;
    }
    }
  }

  if (sessionState != XR_SESSION_STATE_READY && sessionState != XR_SESSION_STATE_SYNCHRONIZED &&
      sessionState != XR_SESSION_STATE_VISIBLE && sessionState != XR_SESSION_STATE_FOCUSED)
  {
    // If we are not ready, synchronized, visible or focused, we skip all processing of this frame
    // This means no waiting, no beginning or ending of the frame at all
    return BeginFrameResult::SkipFully;
  }

  // Wait for the new frame
  pOut->frameState.type = XR_TYPE_FRAME_STATE;
  XrFrameWaitInfo frameWaitInfo{ XR_TYPE_FRAME_WAIT_INFO };
  XrResult result = xrWaitFrame(session, &frameWaitInfo, &pOut->frameState);
  if (XR_FAILED(result))
  {
    util::error(Error::GenericOpenXR);
    return BeginFrameResult::Error;
  }

  
  // Begin the new frame
  XrFrameBeginInfo frameBeginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
  result = xrBeginFrame(session, &frameBeginInfo);
  if (XR_FAILED(result))
  {
    util::error(Error::GenericOpenXR);
    return BeginFrameResult::Error;
  }
  

  if (!pOut->frameState.shouldRender)
  {
    // Let the host know that we don't want to render this frame
    // We do still need to end the frame however
    return BeginFrameResult::SkipRender;
  }

  //std::lock_guard<std::mutex> guard(eyePoseMutex);

  // Update the eye poses
  pOut->viewState.type = XR_TYPE_VIEW_STATE;
  uint32_t viewCount;
  XrViewLocateInfo viewLocateInfo{ XR_TYPE_VIEW_LOCATE_INFO };
  viewLocateInfo.viewConfigurationType = context->getXrViewType();
  viewLocateInfo.displayTime = pOut->frameState.predictedDisplayTime;
  viewLocateInfo.space = space;
  result = xrLocateViews(session, &viewLocateInfo, &pOut->viewState, static_cast<uint32_t>(pOut->eyePoses.size()), &viewCount,
                         pOut->eyePoses.data());
  if (XR_FAILED(result))
  {
    util::error(Error::GenericOpenXR);
    return BeginFrameResult::Error;
  }

  if (viewCount != eyeCount)
  {
    util::error(Error::GenericOpenXR);
    return BeginFrameResult::Error;
  }

  // Update the eye render infos, view and projection matrices
  for (size_t eyeIndex = 0u; eyeIndex < eyeCount; ++eyeIndex)
  {
    // Copy the eye poses into the eye render infos
    XrCompositionLayerProjectionView& eyeRenderInfo = pOut->eyeRenderInfos.at(eyeIndex);
    const XrView& eyePose = pOut->eyePoses.at(eyeIndex);
    eyeRenderInfo.pose = eyePose.pose;
    eyeRenderInfo.fov = eyePose.fov;

    // Update the view and projection matrices
    const XrPosef& pose = eyeRenderInfo.pose;
    pOut->eyeViewMatrices.at(eyeIndex) = util::poseToMatrix(pose);
    pOut->eyeProjectionMatrices.at(eyeIndex) = util::createProjectionMatrix(eyeRenderInfo.fov, 0.1f, 250.0f);
    if (eyeIndex == 0)
    {
      pOut->eyeTangents_l[0] = eyeRenderInfo.fov.angleLeft;
      pOut->eyeTangents_l[1] = eyeRenderInfo.fov.angleRight;
      pOut->eyeTangents_l[2] = eyeRenderInfo.fov.angleUp;
      pOut->eyeTangents_l[3] = eyeRenderInfo.fov.angleDown;
    }
    else {
      pOut->eyeTangents_r[0] = eyeRenderInfo.fov.angleLeft;
      pOut->eyeTangents_r[1] = eyeRenderInfo.fov.angleRight;
      pOut->eyeTangents_r[2] = eyeRenderInfo.fov.angleUp;
      pOut->eyeTangents_r[3] = eyeRenderInfo.fov.angleDown;
    }
  }

  for (int i = 0; i < 64; i++) {
    pOut->tracked_locations[i].pose.position = {0.0, 0.0, 0.0};
    pOut->tracked_locations[i].pose.orientation = {0.0, 0.0, 0.0, 1.0};
  }

  // query each value / location with a subaction path != XR_NULL_PATH
  // resulting in individual values per hand/.

  const XrActiveActionSet active_actionsets[] = {
    {.actionSet = gameplay_actionset, .subactionPath = XR_NULL_PATH}};

  XrActionsSyncInfo actions_sync_info = {
    .type = XR_TYPE_ACTIONS_SYNC_INFO,
    .countActiveActionSets = sizeof(active_actionsets) / sizeof(active_actionsets[0]),
    .activeActionSets = active_actionsets,
  };
  result = xrSyncActions(session, &actions_sync_info);
  //xr_result(self->instance, result, "failed to sync actions!");

  pOut->system_button = false;
  pOut->menu_button = false;
  pOut->left_touch_button = false;
  pOut->right_touch_button = false;

  for (int i = 0; i < HAND_COUNT; i++) {
    XrActionStatePose hand_pose_state = {.type = XR_TYPE_ACTION_STATE_POSE, .next = NULL};
    {
      XrActionStateGetInfo get_info = {.type = XR_TYPE_ACTION_STATE_GET_INFO,
                                       .next = NULL,
                                       .action = hand_pose_action,
                                       .subactionPath = hand_paths[i]};
      result = xrGetActionStatePose(session, &get_info, &hand_pose_state);
      //xr_check(instance, result, "failed to get pose value!");
    }
    //printf("Hand pose %d active: %d\n", i, hand_pose_state.isActive);

    pOut->tracked_locations[i].type = XR_TYPE_SPACE_LOCATION;
    pOut->tracked_locations[i].next = NULL;

    result = xrLocateSpace(hand_pose_spaces[i], space, pOut->frameState.predictedDisplayTime,
                           &pOut->tracked_locations[i]);
    //xr_check(instance, result, "failed to locate space %d!", i);


    /*printf("Pose %d valid %d: %f %f %f %f, %f %f %f\n", i,
    1, hand_locations[i].pose.orientation.x,
    hand_locations[i].pose.orientation.y, hand_locations[i].pose.orientation.z,
    hand_locations[i].pose.orientation.w, hand_locations[i].pose.position.x,
    hand_locations[i].pose.position.y, hand_locations[i].pose.position.z
    );*/


    pOut->grab_value[i].type = XR_TYPE_ACTION_STATE_FLOAT;
    pOut->grab_value[i].next = NULL;
    pOut->grab_value[i].currentState = 0.0;
    {
      XrActionStateGetInfo get_info = {.type = XR_TYPE_ACTION_STATE_GET_INFO,
                                       .next = NULL,
                                       .action = grab_action_float,
                                       .subactionPath = hand_paths[i]};

      result = xrGetActionStateFloat(session, &get_info, &pOut->grab_value[i]);
      //xr_check(instance, result, "failed to get grab value!");
    }

    pOut->grip_value[i].type = XR_TYPE_ACTION_STATE_FLOAT;
    pOut->grip_value[i].next = NULL;
    pOut->grip_value[i].currentState = 0.0;
    {
      XrActionStateGetInfo get_info = {.type = XR_TYPE_ACTION_STATE_GET_INFO,
                                       .next = NULL,
                                       .action = grip_action_float,
                                       .subactionPath = hand_paths[i]};

      result = xrGetActionStateFloat(session, &get_info, &pOut->grip_value[i]);
      //xr_check(instance, result, "failed to get grab value!");
    }

    // printf("Grab %d active %d, current %f, changed %d\n", i,
    // grabValue[i].isActive, grabValue[i].currentState,
    // grabValue[i].changedSinceLastSync);

    pOut->system_value[i].type = XR_TYPE_ACTION_STATE_BOOLEAN;
    pOut->system_value[i].next = NULL;
    {
      XrActionStateGetInfo get_info = {.type = XR_TYPE_ACTION_STATE_GET_INFO,
                                       .next = NULL,
                                       .action = system_action_bool,
                                       .subactionPath = hand_paths[i]};

      
      result = xrGetActionStateBoolean(session, &get_info, &pOut->system_value[i]);
      //xr_check(instance, result, "failed to get grab value!");

      //printf("system %u, %x\n", i, system_value[i].currentState);
      if (pOut->system_value[i].currentState && i == HAND_LEFT_INDEX) {
        pOut->menu_button = true;
      }
      else if (pOut->system_value[i].currentState && i == HAND_RIGHT_INDEX) {
        pOut->system_button = true;
      }
    }

    pOut->b_y_value[i].type = XR_TYPE_ACTION_STATE_BOOLEAN;
    pOut->b_y_value[i].next = NULL;
    {
      XrActionStateGetInfo get_info = {.type = XR_TYPE_ACTION_STATE_GET_INFO,
                                       .next = NULL,
                                       .action = b_y_action_bool,
                                       .subactionPath = hand_paths[i]};

      
      result = xrGetActionStateBoolean(session, &get_info, &pOut->b_y_value[i]);
      //xr_check(instance, result, "failed to get grab value!");

      //printf("system %u, %x\n", i, system_value[i].currentState);
      if (pOut->b_y_value[i].currentState && i == HAND_LEFT_INDEX) {
        pOut->left_touch_button = true;
      }
      else if (pOut->b_y_value[i].currentState && i == HAND_RIGHT_INDEX) {
        pOut->right_touch_button = true;
      }
    }
  };

  XrHandJointsLocateInfoEXT locateInfo{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
  locateInfo.baseSpace = space;
  locateInfo.time = pOut->frameState.predictedDisplayTime;

  if (left_hand_valid)
    context->xrLocateHandJointsEXT(leftHandTracker, &locateInfo, &pOut->leftLocations);
  if (right_hand_valid)
    context->xrLocateHandJointsEXT(rightHandTracker, &locateInfo, &pOut->rightLocations);

  if (pOut->leftLocations.isActive) {
      // The returned joint location array can be directly indexed with
      // XrHandJointEXT enum.
      const XrPosef &indexTipInWorld =
          pOut->leftJointLocations[XR_HAND_JOINT_INDEX_TIP_EXT].pose;
      const XrPosef &thumbTipInWorld =
          pOut->leftJointLocations[XR_HAND_JOINT_THUMB_TIP_EXT].pose;
        const XrPosef &palmInWorld =
          pOut->leftJointLocations[XR_HAND_JOINT_WRIST_EXT].pose;

      // using the returned radius and velocity of index finger tip.
      const float indexTipRadius =
          pOut->leftJointLocations[XR_HAND_JOINT_INDEX_TIP_EXT].radius;
      const XrHandJointVelocityEXT &indexTipVelocity =
          pOut->leftJointVelocities[XR_HAND_JOINT_INDEX_TIP_EXT];

      for (int i = 0; i <= XR_HAND_JOINT_LITTLE_TIP_EXT; i++) {
        pOut->tracked_locations[2+i].pose = pOut->leftJointLocations[i].pose;
      }
      
      glm::vec3 v1 = {indexTipInWorld.position.x, indexTipInWorld.position.y, indexTipInWorld.position.z};
      glm::vec3 v2 = {thumbTipInWorld.position.x, thumbTipInWorld.position.y, thumbTipInWorld.position.z};

      float distance = glm::length(v2 - v1);

      pOut->pinch_l = distance < 0.01;

      //printf("l: %f\n", distance);

      //printf("l: %f %f %f\n", palmInWorld.position.x, palmInWorld.position.y, palmInWorld.position.z);
  }

  if (pOut->rightLocations.isActive) {
      const XrPosef &indexTipInWorld =
          pOut->rightJointLocations[XR_HAND_JOINT_INDEX_TIP_EXT].pose;
      const XrPosef &thumbTipInWorld =
          pOut->rightJointLocations[XR_HAND_JOINT_THUMB_TIP_EXT].pose;
        const XrPosef &palmInWorld =
          pOut->rightJointLocations[XR_HAND_JOINT_WRIST_EXT].pose;

      for (int i = 0; i <= XR_HAND_JOINT_LITTLE_TIP_EXT; i++) {
        pOut->tracked_locations[2+XR_HAND_JOINT_LITTLE_TIP_EXT+1+i].pose = pOut->rightJointLocations[i].pose;
      }

      glm::vec3 v1 = {indexTipInWorld.position.x, indexTipInWorld.position.y, indexTipInWorld.position.z};
      glm::vec3 v2 = {thumbTipInWorld.position.x, thumbTipInWorld.position.y, thumbTipInWorld.position.z};

      float distance = glm::length(v2 - v1);

      pOut->pinch_r = distance < 0.01;

      //printf("r: %f\n", distance);

      //printf("r: %f %f %f\n", palmInWorld.position.x, palmInWorld.position.y, palmInWorld.position.z);
  }

  //printf("Left 1? %f %f %f %f\n", ql_xrsp_sidechannel_eye_l_orient[0], ql_xrsp_sidechannel_eye_l_orient[1], ql_xrsp_sidechannel_eye_l_orient[2], ql_xrsp_sidechannel_eye_l_orient[3]);
  glm::quat l_q = eye_orientation("ql_xrsp_sidechannel_eye_l_orient");
  pOut->l_eye_mat = glm::toMat4(l_q);
  glm::mat4 l_view_no_rot = pOut->eyeViewMatrices.at(0);
  l_view_no_rot[3] = glm::vec4(0, 0, 0, 1);
  pOut->l_eye_mat = l_view_no_rot * pOut->l_eye_mat;
  pOut->l_eye_quat = l_q;

  glm::quat r_q = eye_orientation("ql_xrsp_sidechannel_eye_r_orient");
  pOut->r_eye_mat = glm::toMat4(r_q);
  glm::mat4 r_view_no_rot = pOut->eyeViewMatrices.at(1);
  r_view_no_rot[3] = glm::vec4(0, 0, 0, 1);
  pOut->r_eye_mat = r_view_no_rot * pOut->r_eye_mat;

  return BeginFrameResult::RenderFully; // Request full rendering of the frame
}

void Headset::beginFrameRender(uint32_t& swapchainImageIndex)
{
  XrResult result;
  /*// Begin the new frame
  XrFrameBeginInfo frameBeginInfo{ XR_TYPE_FRAME_BEGIN_INFO };
  result = xrBeginFrame(session, &frameBeginInfo);
  if (XR_FAILED(result))
  {
    util::error(Error::GenericOpenXR);
    return; //BeginFrameResult::Error;
  }*/


  // Acquire the swapchain image
  XrSwapchainImageAcquireInfo swapchainImageAcquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
  result = xrAcquireSwapchainImage(swapchain, &swapchainImageAcquireInfo, &swapchainImageIndex);
  if (XR_FAILED(result))
  {
    util::error(Error::GenericOpenXR);
    return;// BeginFrameResult::Error;
  }

  // Wait for the swapchain image
  XrSwapchainImageWaitInfo swapchainImageWaitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
  swapchainImageWaitInfo.timeout = 1000000;
  result = xrWaitSwapchainImage(swapchain, &swapchainImageWaitInfo);
  if (XR_FAILED(result))
  {
    util::error(Error::GenericOpenXR);
    return;// BeginFrameResult::Error;
  }
}

void Headset::endRender(int poseIdx) const
{
  // Release the swapchain image
  XrSwapchainImageReleaseInfo swapchainImageReleaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
  XrResult result = xrReleaseSwapchainImage(swapchain, &swapchainImageReleaseInfo);
  if (XR_FAILED(result))
  {
    return;
  }
}

void Headset::endFrame(int poseIdx) const
{
  // End the frame
  XrCompositionLayerProjection compositionLayerProjection{ XR_TYPE_COMPOSITION_LAYER_PROJECTION };
  compositionLayerProjection.space = space;
  compositionLayerProjection.viewCount = static_cast<uint32_t>(storedPoses[poseIdx].eyeRenderInfos.size());
  auto submittedViews = storedPoses[poseIdx].eyeRenderInfos;
  float monoAngles[4];
  if (!submittedViews.empty() && openxr_get_mono_fov(monoAngles)) {
    // Both textures contain the same simulator camera image. Describe that
    // camera identically for each eye instead of inventing stereo disparity.
    const XrPosef cameraPose = submittedViews[0].pose;
    const XrFovf cameraFov{monoAngles[0], monoAngles[1], monoAngles[2], monoAngles[3]};
    for (auto &view : submittedViews) {
      view.pose = cameraPose;
      view.fov = cameraFov;
    }
  }
  compositionLayerProjection.views = submittedViews.data();

  std::vector<XrCompositionLayerBaseHeader*> layers;

  const bool positionValid = storedPoses[poseIdx].viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT;
  const bool orientationValid = storedPoses[poseIdx].viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  if (storedPoses[poseIdx].frameState.shouldRender && positionValid && orientationValid)
  {
    layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&compositionLayerProjection));
  }

  XrFrameEndInfo frameEndInfo{ XR_TYPE_FRAME_END_INFO };
  frameEndInfo.displayTime = storedPoses[poseIdx].frameState.predictedDisplayTime;
  frameEndInfo.layerCount = static_cast<uint32_t>(layers.size());
  frameEndInfo.layers = layers.data();
  frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  XrResult result = xrEndFrame(session, &frameEndInfo);
  if (XR_FAILED(result))
  {
    return;
  }
}

bool Headset::isValid() const
{
  return valid;
}

bool Headset::isExitRequested() const
{
  return exitRequested;
}

size_t Headset::getEyeCount() const
{
  return eyeCount;
}

VkExtent2D Headset::getEyeResolution(size_t eyeIndex) const
{
  const XrViewConfigurationView& eyeInfo = eyeImageInfos.at(eyeIndex);
  return { eyeInfo.recommendedImageRectWidth, eyeInfo.recommendedImageRectHeight };
}

RenderTarget* Headset::getRenderTarget(size_t swapchainImageIndex) const
{
  return swapchainRenderTargets.at(swapchainImageIndex);
}

bool Headset::beginSession() const
{
  // Start the session
  XrSessionBeginInfo sessionBeginInfo{ XR_TYPE_SESSION_BEGIN_INFO };
  sessionBeginInfo.primaryViewConfigurationType = context->getXrViewType();
  const XrResult result = xrBeginSession(session, &sessionBeginInfo);
  if (XR_FAILED(result))
  {
    util::error(Error::GenericOpenXR);
    return false;
  }

  return true;
}

bool Headset::endSession() const
{
  // End the session
  const XrResult result = xrEndSession(session);
  if (XR_FAILED(result))
  {
    util::error(Error::GenericOpenXR);
    return false;
  }

  return true;
}
