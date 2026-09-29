#pragma once

#include "baseclasses/Mechanics.h"
#include "baseclasses/Pipelines.h"
#include "baseclasses/Resources.h"
#include "baseclasses/Swapchain.h"

#include <cstdint>
#include <span>
#include <vector>

namespace VP {

// One dispatch and the buffers it touches, as its shader's qualifiers declare them.
struct Pass {
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorSet block = VK_NULL_HANDLE;
  std::uint32_t groups = 0;
  std::vector<VkBuffer> reads;
  std::vector<VkBuffer> writes;
};

// The top of baseclasses: owns the GPU and runs one frame of passes. Barriers follow
// from what each pass declares it reads and writes, so nobody places them by hand.
class Engine {
public:
  explicit Engine(const Log &log);
  ~Engine();

  const Resources &resources() const;
  const Pipelines &pipelines() const;
  void wait() const;
  // Zeroes the new buffers first, so a run starts from the same bytes every time (C01).
  void run(std::span<const VkBuffer> clears, std::span<const Pass> passes);

private:
  bool hazard(const Pass &pass) const;

  Mechanics _mechanics;
  Resources _resources;
  Pipelines _pipelines;
  // Since the last barrier; kept between frames so a frame allocates nothing (CPP10).
  std::vector<VkBuffer> _written;
  std::vector<VkBuffer> _read;
};

} // namespace VP
