#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace VP {

class Log;
class Window;

// Throws with the call's name, so a Vulkan error names its cause (A02).
void check(VkResult result, const char *call);

// The GPU machine: instance, device and queues. It needs no window to start (V07), yet a
// window can open at any time: the instance takes every surface extension the loader
// offers, and the device its swapchain where it has one. A window at start steers the
// choice to a device that presents to it.
class Mechanics {
public:
  Mechanics(const Log &log, const Window *window);
  ~Mechanics();
  Mechanics(const Mechanics &) = delete;
  Mechanics &operator=(const Mechanics &) = delete;

  VkInstance instance() const;
  VkPhysicalDevice physical_device() const;
  VkDevice device() const;
  VkQueue queue() const;
  // Whether the device's one queue can draw frames and show them on the window this
  // surface is of.
  bool presents(VkSurfaceKHR surface) const;

  // One frame in flight: the frame loop waits on nothing but this fence (VK02).
  void wait() const;
  void wait_idle() const;
  VkCommandBuffer record() const;
  // With a window image: waits for it to be acquired before writing it, and signals
  // once it is rendered.
  void submit(VkSemaphore acquired = VK_NULL_HANDLE,
              VkSemaphore rendered = VK_NULL_HANDLE) const;

private:
  // The GPU, and the family of the one queue that runs everything.
  struct Choice {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    std::uint32_t family = 0;
  };

  static Choice choose(VkInstance instance, const Window *window, const Log &log);

  const VkInstance _instance;
  const Choice _choice;
  const VkDevice _device;
  VkQueue _queue = VK_NULL_HANDLE;
  VkCommandPool _command_pool = VK_NULL_HANDLE;
  VkCommandBuffer _commands = VK_NULL_HANDLE;
  VkFence _fence = VK_NULL_HANDLE;
};

} // namespace VP
