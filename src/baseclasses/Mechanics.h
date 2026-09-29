#pragma once

#include "baseclasses/Log.h"

#include <vulkan/vulkan.h>

#include <cstdint>

namespace VP {

// Throws with the call's name, so a Vulkan error names its cause (A02).
void check(VkResult result, const char *call);

// The GPU machine: instance, device and queues. It knows no window, so a headless run
// needs nothing else.
class Mechanics {
public:
  explicit Mechanics(const Log &log);
  ~Mechanics();
  Mechanics(const Mechanics &) = delete;
  Mechanics &operator=(const Mechanics &) = delete;

  VkInstance instance() const;
  VkPhysicalDevice physical_device() const;
  VkDevice device() const;

  // One frame in flight: the frame loop waits on nothing but this fence (VK02).
  void wait() const;
  void wait_idle() const;
  VkCommandBuffer record() const;
  void submit() const;

private:
  VkInstance _instance = VK_NULL_HANDLE;
  VkPhysicalDevice _physical_device = VK_NULL_HANDLE;
  std::uint32_t _queue_family = 0;
  VkDevice _device = VK_NULL_HANDLE;
  VkQueue _queue = VK_NULL_HANDLE;
  VkCommandPool _command_pool = VK_NULL_HANDLE;
  VkCommandBuffer _commands = VK_NULL_HANDLE;
  VkFence _fence = VK_NULL_HANDLE;
};

} // namespace VP
