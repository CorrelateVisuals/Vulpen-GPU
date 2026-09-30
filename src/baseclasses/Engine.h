#pragma once

#include "baseclasses/Mechanics.h"
#include "baseclasses/Passes.h"
#include "baseclasses/Pipelines.h"
#include "baseclasses/Resources.h"
#include "baseclasses/Swapchain.h"

#include <optional>
#include <span>

namespace VP {

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
  void dispatch(VkCommandBuffer commands, std::span<const Pass> passes);
  void draw(VkCommandBuffer commands,
            const Target &target,
            std::span<const Pass> passes) const;

  Mechanics _mechanics;
  Resources _resources;
  Pipelines _pipelines;
  std::optional<Swapchain> _swapchain;
  Hazards _hazards;
};

} // namespace VP
