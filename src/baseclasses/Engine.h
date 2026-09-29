#pragma once

#include "baseclasses/Mechanics.h"
#include "baseclasses/Pipelines.h"
#include "baseclasses/Resources.h"
#include "baseclasses/Swapchain.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace VP {

// One dispatch or draw and the buffers it touches, as its shaders' qualifiers declare
// them.
struct Pass {
  VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorSet block = VK_NULL_HANDLE;
  std::uint32_t groups = 0;       // a dispatch's
  std::uint32_t vertex_count = 0; // a draw's
  std::vector<VkBuffer> reads;
  std::vector<VkBuffer> writes;
};

// The top of baseclasses: owns the GPU and runs one frame of passes. Barriers follow
// from what each pass declares it reads and writes, so nobody places them by hand.
class Engine {
public:
  // Without a window the engine only dispatches (V07); with one, each frame also draws
  // into the window and presents.
  Engine(const Log &log, const Window *window);
  ~Engine();

  const Resources &resources() const;
  const Pipelines &pipelines() const;
  // What draws render into; null without a window.
  VkRenderPass render_pass() const;
  void wait() const;
  // Zeroes the new buffers first, so a run starts from the same bytes every time (C01).
  // Dispatches run first, in order; draws, which only read, follow in one render pass.
  void run(std::span<const VkBuffer> clears, std::span<const Pass> passes);

private:
  bool hazard(const Pass &pass) const;
  void dispatch(VkCommandBuffer commands, std::span<const Pass> passes);
  void draw(VkCommandBuffer commands,
            const Target &target,
            std::span<const Pass> passes) const;

  Mechanics _mechanics;
  Resources _resources;
  Pipelines _pipelines;
  std::optional<Swapchain> _swapchain;
  // Since the last barrier; kept between frames so a frame allocates nothing (CPP10).
  std::vector<VkBuffer> _written;
  std::vector<VkBuffer> _read;
};

} // namespace VP
