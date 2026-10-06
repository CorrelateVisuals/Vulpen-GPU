#pragma once

#include "baseclasses/Passes.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace VP {

class Log;
class Pipelines;
class Resources;
class Window;

// The top of baseclasses: owns the GPU and runs one frame of passes. Barriers follow
// from what each pass declares it reads and writes, so nobody places them by hand.
class Engine {
public:
  // Without a window the engine only dispatches (V07); with one, each frame also draws
  // into the window and presents.
  Engine(const Log &log, const Window *window);
  ~Engine();
  Engine(const Engine &) = delete;
  Engine &operator=(const Engine &) = delete;

  const Resources &resources() const;
  const Pipelines &pipelines() const;
  // The window output, open while a node draws: the window must outlive it. Between
  // frames only, since closing waits for the GPU.
  void open(const Window &window);
  void close();
  // What draws render into; null without a window.
  VkRenderPass render_pass() const;
  void wait() const;
  // Zeroes the new buffers first, so a run starts from the same bytes every time (C01),
  // and copies in the images the CPU filled. Dispatches run next, in order; draws, which
  // only read, follow in one render pass. frame, time and cursor: what the frame block
  // tells every pass (RV02).
  void run(std::span<const VkBuffer> clears,
           std::span<const Copy> copies,
           std::span<const Pass> passes,
           std::uint64_t frame,
           double time,
           std::array<float, 2> cursor);

private:
  // What the engine owns, behind a pointer, so a file that includes this header
  // compiles none of the rest of baseclasses.
  struct Gpu;
  std::unique_ptr<Gpu> _gpu;
};

} // namespace VP
