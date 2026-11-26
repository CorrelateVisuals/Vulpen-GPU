#pragma once

#include "baseclasses/Owned.h"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace VP {

class Image;
class Log;
class Mechanics;
class Resources;
class Window;

// "Looking for that blessed hope, and the glorious appearing of the great God and our
// Saviour Jesus Christ;" Titus 2:13

// The window image one frame renders into, and what orders the frame around it.
struct Target {
  std::uint32_t image = 0;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkExtent2D extent{};
  VkSemaphore acquired = VK_NULL_HANDLE;
  VkSemaphore rendered = VK_NULL_HANDLE;
};

// The window output: one output next to files and streams, never needed to run. It
// remakes its images when the window's size changes, a depth image with them, which the
// window's draws share; its render pass stays, so the draws' pipelines stay too, until
// the samples a pixel change.
class Swapchain {
public:
  // Makes the window's surface. Throws, naming why, when the device cannot present to
  // it. The window must outlive it. samples: a pixel's, as samples() takes them; vsync:
  // as vsync() takes it.
  Swapchain(const Log &log,
            const Mechanics &mechanics,
            const Resources &resources,
            const Window &window,
            VkSampleCountFlagBits samples,
            bool vsync);
  ~Swapchain();
  Swapchain(const Swapchain &) = delete;
  Swapchain &operator=(const Swapchain &) = delete;

  VkRenderPass render_pass() const;
  // With vsync, FIFO, a frame each refresh; without, mailbox where the window offers it,
  // which never holds the frame loop for the display (VK02), else FIFO. The images are
  // made again as the next frame acquires one. The mode it presents in.
  VkPresentModeKHR vsync(bool on);
  // The draws go into images of that many samples a pixel, resolved into the window's
  // as the render pass ends. Between frames: it makes its render pass again, so every
  // pipeline that draws into the window is made again too.
  void samples(VkSampleCountFlagBits samples);
  // Nothing while the window is minimized, or while its images are being remade.
  std::optional<Target> acquire();
  // Starts the render pass on the target, clearing it and the depth.
  void begin(VkCommandBuffer commands, const Target &target) const;
  void present(const Target &target);

private:
  void presents() const;
  void make();
  void make_images();
  void release();

  const Log &_log;
  const Resources &_resources;
  const Window &_window;
  const VkPhysicalDevice _physical_device;
  const VkDevice _device;
  const VkQueue _queue;
  const Owned<VkSurfaceKHR, vkDestroySurfaceKHR, VkInstance> _surface;
  const VkSurfaceFormatKHR _format;
  VkPresentModeKHR _present_mode;
  VkSampleCountFlagBits _samples;
  Owned<VkRenderPass, vkDestroyRenderPass, VkDevice> _render_pass;
  // One frame in flight, so one is enough.
  const Owned<VkSemaphore, vkDestroySemaphore, VkDevice> _acquired;
  Owned<VkSwapchainKHR, vkDestroySwapchainKHR, VkDevice> _swapchain;
  VkExtent2D _extent{};
  VkExtent2D _made_for{}; // the window's size then, which _extent may be clamped from
  bool _stale = false;
  bool _untold = false; // a new mode or new samples, till the next frame says them
  std::vector<Owned<VkImageView, vkDestroyImageView, VkDevice>> _views;
  std::unique_ptr<Image> _depth; // the window's size, as its images are
  std::unique_ptr<Image> _color; // with samples past one, what the draws go into
  std::vector<Owned<VkFramebuffer, vkDestroyFramebuffer, VkDevice>> _framebuffers;
  // One per image: presenting holds its semaphore until that image shows, which no
  // fence tells the CPU.
  std::vector<Owned<VkSemaphore, vkDestroySemaphore, VkDevice>> _rendered;
};

} // namespace VP
