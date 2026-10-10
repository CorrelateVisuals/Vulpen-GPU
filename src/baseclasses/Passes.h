#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace VP {

// An image a draw renders into instead of the window: its format's render pass, its
// framebuffer and its size. With no framebuffer, as while it has no size, the draw
// records nothing.
struct Offscreen {
  VkRenderPass render_pass = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkExtent2D extent{};
};

// One dispatch or draw and the buffers it touches, as its shaders' qualifiers declare
// them.
struct Pass {
  VkPipelineBindPoint bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorSet block = VK_NULL_HANDLE;
  std::uint32_t groups = 0;       // a dispatch's
  std::uint32_t vertex_count = 0; // a draw's, of each instance
  std::uint32_t instance_count = 1;
  // When set, the instance count, which the CPU sets each frame as it writes a buffer.
  const std::uint32_t *instances = nullptr;
  // A draw's image, which its schedule keeps and remakes as the size changes; null for a
  // draw into the window.
  const Offscreen *offscreen = nullptr;
  std::vector<VkBuffer> reads;
  std::vector<VkBuffer> writes;
};

// Pixels the CPU put in a buffer, which the frame copies into an image before its
// passes, so any of them may sample it.
struct Copy {
  VkBuffer from = VK_NULL_HANDLE;
  VkImage to = VK_NULL_HANDLE;
  VkExtent2D extent{};
};

// Where barriers go between passes (V10): a pass waits for what an earlier pass wrote,
// and for earlier reads of what it writes. It stands apart from the engine so a test can
// check it without a GPU: synchronization validation cannot see accesses through buffer
// addresses.
class Hazards {
public:
  // A frame's first pass waits for nothing: the previous frame's fence covered it.
  void clear();
  // Whether a barrier goes before the pass; either way, the pass counts from then on.
  bool before(const Pass &pass);

private:
  // Since the last barrier; kept between frames so a frame allocates nothing (CPP10).
  std::vector<VkBuffer> _written;
  std::vector<VkBuffer> _read;
};

} // namespace VP
