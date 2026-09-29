#include "baseclasses/Swapchain.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace VP {

namespace {

constexpr auto no_timeout = std::numeric_limits<std::uint64_t>::max();
// One image more than the minimum, so acquiring one rarely waits for the display.
constexpr std::uint32_t spare_images = 1;
constexpr VkClearColorValue clear_color{.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
constexpr float far_depth = 1.0f;
// The width a surface reports when the swapchain picks its size (VK_KHR_surface).
constexpr std::uint32_t size_from_swapchain = std::numeric_limits<std::uint32_t>::max();

// sRGB, so shaders write linear color and the display gets it encoded.
VkSurfaceFormatKHR pick_format(VkPhysicalDevice device, VkSurfaceKHR surface) {
  std::uint32_t count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, formats.data());
  if (formats.empty())
    throw std::runtime_error("the window's surface offers no image format");
  const auto srgb = std::ranges::find_if(formats, [](const VkSurfaceFormatKHR &format) {
    return format.format == VK_FORMAT_B8G8R8A8_SRGB &&
           format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  });
  return srgb != formats.end() ? *srgb : formats.front();
}

// Mailbox never holds the frame loop for the display (VK02); FIFO, which every device
// offers, may.
VkPresentModeKHR pick_present_mode(VkPhysicalDevice device, VkSurfaceKHR surface) {
  std::uint32_t count = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, nullptr);
  std::vector<VkPresentModeKHR> modes(count);
  vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, modes.data());
  return std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()
             ? VK_PRESENT_MODE_MAILBOX_KHR
             : VK_PRESENT_MODE_FIFO_KHR;
}

VkRenderPass make_render_pass(VkDevice device, VkFormat format) {
  const VkAttachmentDescription color{.format = format,
                                      .samples = VK_SAMPLE_COUNT_1_BIT,
                                      .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                      .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                      .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                      .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
  const VkAttachmentReference target{.attachment = 0,
                                     .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  const VkSubpassDescription subpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                     .colorAttachmentCount = 1,
                                     .pColorAttachments = &target};
  // The image's layout changes only once it is acquired, which the submit waits for at
  // this stage.
  const VkSubpassDependency acquired{
      .srcSubpass = VK_SUBPASS_EXTERNAL,
      .dstSubpass = 0,
      .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT};
  const VkRenderPassCreateInfo info{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                    .attachmentCount = 1,
                                    .pAttachments = &color,
                                    .subpassCount = 1,
                                    .pSubpasses = &subpass,
                                    .dependencyCount = 1,
                                    .pDependencies = &acquired};
  VkRenderPass render_pass = VK_NULL_HANDLE;
  check(vkCreateRenderPass(device, &info, nullptr, &render_pass), "vkCreateRenderPass");
  return render_pass;
}

VkSemaphore make_semaphore(VkDevice device) {
  const VkSemaphoreCreateInfo info{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  VkSemaphore semaphore = VK_NULL_HANDLE;
  check(vkCreateSemaphore(device, &info, nullptr, &semaphore), "vkCreateSemaphore");
  return semaphore;
}

} // namespace

Swapchain::Swapchain(const Mechanics &mechanics, const Window &window)
    : _mechanics(mechanics), _window(window), _device(mechanics.device()),
      _format(pick_format(mechanics.physical_device(), mechanics.surface())),
      _present_mode(pick_present_mode(mechanics.physical_device(), mechanics.surface())),
      _render_pass(make_render_pass(_device, _format.format)),
      _acquired(make_semaphore(_device)) {
  make();
}

Swapchain::~Swapchain() {
  _mechanics.wait_idle();
  release();
  vkDestroySwapchainKHR(_device, _swapchain, nullptr);
  vkDestroySemaphore(_device, _acquired, nullptr);
  vkDestroyRenderPass(_device, _render_pass, nullptr);
}

VkRenderPass Swapchain::render_pass() const {
  return _render_pass;
}

std::optional<Target> Swapchain::acquire() {
  const VkExtent2D size = _window.size();
  if (size.width == 0 || size.height == 0)
    return std::nullopt;
  if (_stale || size.width != _made_for.width || size.height != _made_for.height) {
    _mechanics.wait_idle();
    release();
    make();
  }
  std::uint32_t image = 0;
  const VkResult result = vkAcquireNextImageKHR(
      _device, _swapchain, no_timeout, _acquired, VK_NULL_HANDLE, &image);
  // Suboptimal still hands over an image, so this frame draws and the next remakes.
  _stale = result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR;
  if (result == VK_ERROR_OUT_OF_DATE_KHR)
    return std::nullopt;
  if (result != VK_SUBOPTIMAL_KHR)
    check(result, "vkAcquireNextImageKHR");
  return Target{.image = image,
                .framebuffer = _framebuffers[image],
                .extent = _extent,
                .acquired = _acquired,
                .rendered = _rendered[image]};
}

void Swapchain::begin(VkCommandBuffer commands, const Target &target) const {
  const VkClearValue clear{.color = clear_color};
  const VkRenderPassBeginInfo info{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                   .renderPass = _render_pass,
                                   .framebuffer = target.framebuffer,
                                   .renderArea = {.extent = target.extent},
                                   .clearValueCount = 1,
                                   .pClearValues = &clear};
  vkCmdBeginRenderPass(commands, &info, VK_SUBPASS_CONTENTS_INLINE);
  const VkViewport viewport{.width = static_cast<float>(target.extent.width),
                            .height = static_cast<float>(target.extent.height),
                            .maxDepth = far_depth};
  const VkRect2D scissor{.extent = target.extent};
  vkCmdSetViewport(commands, 0, 1, &viewport);
  vkCmdSetScissor(commands, 0, 1, &scissor);
}

void Swapchain::present(const Target &target) {
  const VkResult result = _mechanics.present(_swapchain, target.image, target.rendered);
  if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR)
    _stale = true;
  else
    check(result, "vkQueuePresentKHR");
}

// The old swapchain, if any, hands its images over to the new one.
void Swapchain::make() {
  VkSurfaceCapabilitiesKHR capabilities{};
  check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
            _mechanics.physical_device(), _mechanics.surface(), &capabilities),
        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
  _made_for = _window.size();
  _extent = capabilities.currentExtent;
  if (_extent.width == size_from_swapchain)
    _extent = {std::clamp(_made_for.width,
                          capabilities.minImageExtent.width,
                          capabilities.maxImageExtent.width),
               std::clamp(_made_for.height,
                          capabilities.minImageExtent.height,
                          capabilities.maxImageExtent.height)};
  const std::uint32_t wanted = capabilities.minImageCount + spare_images;
  const std::uint32_t images = capabilities.maxImageCount == 0
                                   ? wanted
                                   : std::min(wanted, capabilities.maxImageCount);
  const VkCompositeAlphaFlagsKHR alphas = capabilities.supportedCompositeAlpha;
  const VkSwapchainKHR old = _swapchain;
  const VkSwapchainCreateInfoKHR info{
      .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
      .surface = _mechanics.surface(),
      .minImageCount = images,
      .imageFormat = _format.format,
      .imageColorSpace = _format.colorSpace,
      .imageExtent = _extent,
      .imageArrayLayers = 1,
      .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
      .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .preTransform = capabilities.currentTransform,
      // Opaque where offered; otherwise the lowest mode the surface offers.
      .compositeAlpha = (alphas & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                            : static_cast<VkCompositeAlphaFlagBitsKHR>(alphas & -alphas),
      .presentMode = _present_mode,
      .clipped = VK_TRUE,
      .oldSwapchain = old};
  check(vkCreateSwapchainKHR(_device, &info, nullptr, &_swapchain),
        "vkCreateSwapchainKHR");
  vkDestroySwapchainKHR(_device, old, nullptr);
  _stale = false;
  make_images();
}

void Swapchain::make_images() {
  std::uint32_t count = 0;
  vkGetSwapchainImagesKHR(_device, _swapchain, &count, nullptr);
  std::vector<VkImage> images(count);
  vkGetSwapchainImagesKHR(_device, _swapchain, &count, images.data());
  for (const VkImage image : images) {
    const VkImageViewCreateInfo view{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = _format.format,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1,
                             .layerCount = 1}};
    check(vkCreateImageView(_device, &view, nullptr, &_views.emplace_back()),
          "vkCreateImageView");
    const VkFramebufferCreateInfo framebuffer{
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = _render_pass,
        .attachmentCount = 1,
        .pAttachments = &_views.back(),
        .width = _extent.width,
        .height = _extent.height,
        .layers = 1};
    check(vkCreateFramebuffer(
              _device, &framebuffer, nullptr, &_framebuffers.emplace_back()),
          "vkCreateFramebuffer");
    _rendered.push_back(make_semaphore(_device));
  }
}

// Everything sized by the swapchain but the swapchain, which the next make() retires.
void Swapchain::release() {
  for (const VkSemaphore semaphore : _rendered)
    vkDestroySemaphore(_device, semaphore, nullptr);
  for (const VkFramebuffer framebuffer : _framebuffers)
    vkDestroyFramebuffer(_device, framebuffer, nullptr);
  for (const VkImageView view : _views)
    vkDestroyImageView(_device, view, nullptr);
  _rendered.clear();
  _framebuffers.clear();
  _views.clear();
}

} // namespace VP
