#pragma once

#include "baseclasses/Platform.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace VP {

// The window image one frame renders into, and what orders the frame around it.
struct Target {
  std::uint32_t image = 0;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkExtent2D extent{};
  VkSemaphore acquired = VK_NULL_HANDLE;
  VkSemaphore rendered = VK_NULL_HANDLE;
};

// The window output: one output next to files and streams, never needed to run. It
// remakes its images when the window's size changes; its render pass stays, so the
// draws' pipelines stay too.
class Swapchain {
public:
  Swapchain(VkPhysicalDevice physical_device,
            VkDevice device,
            VkQueue queue,
            VkSurfaceKHR surface,
            const Window &window);
  ~Swapchain();
  Swapchain(const Swapchain &) = delete;
  Swapchain &operator=(const Swapchain &) = delete;

  VkRenderPass render_pass() const;
  // Nothing while the window is minimized, or while its images are being remade.
  std::optional<Target> acquire();
  // Starts the render pass on the target, clearing it, with the viewport filling it.
  void begin(VkCommandBuffer commands, const Target &target) const;
  void present(const Target &target);

private:
  void make();
  void make_images();
  void release();

  const Window &_window;
  const VkPhysicalDevice _physical_device;
  const VkDevice _device;
  const VkQueue _queue;
  const VkSurfaceKHR _surface;
  const VkSurfaceFormatKHR _format;
  const VkPresentModeKHR _present_mode;
  const VkRenderPass _render_pass;
  const VkSemaphore _acquired; // one frame in flight, so one is enough
  VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
  VkExtent2D _extent{};
  VkExtent2D _made_for{}; // the window's size then, which _extent may be clamped from
  bool _stale = false;
  std::vector<VkImageView> _views;
  std::vector<VkFramebuffer> _framebuffers;
  // One per image: presenting holds its semaphore until that image shows, which no
  // fence tells the CPU.
  std::vector<VkSemaphore> _rendered;
};

} // namespace VP
