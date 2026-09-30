#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

namespace VP {

class Log;
class Window;

// Throws with the call's name, so a Vulkan error names its cause (A02).
void check(VkResult result, const char *call);

// The GPU machine: instance, device and queues. A window is optional, so a headless run
// needs nothing else; with one, the device is one that presents to it.
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
  // Null without a window.
  VkSurfaceKHR surface() const;

  // One frame in flight: the frame loop waits on nothing but this fence (VK02).
  void wait() const;
  void wait_idle() const;
  VkCommandBuffer record() const;
  // With a window image: waits for it to be acquired before writing it, and signals
  // once it is rendered.
  void submit(VkSemaphore acquired = VK_NULL_HANDLE,
              VkSemaphore rendered = VK_NULL_HANDLE) const;

private:
  const VkInstance _instance;
  const VkSurfaceKHR _surface;
  const VkPhysicalDevice _physical_device;
  const std::uint32_t _queue_family;
  const VkDevice _device;
  VkQueue _queue = VK_NULL_HANDLE;
  VkCommandPool _command_pool = VK_NULL_HANDLE;
  VkCommandBuffer _commands = VK_NULL_HANDLE;
  VkFence _fence = VK_NULL_HANDLE;
};

} // namespace VP
