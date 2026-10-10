#pragma once

#include "baseclasses/Resources.h"
#include "baseclasses/Shader.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace VP {

// Where every image a shader samples lives, and the static samplers, which are
// samplers[0] to samplers[3] (RV02).
inline constexpr std::uint32_t images_set = 0;
inline constexpr std::uint32_t static_samplers = 4;

// What the GPU runs: the one pipeline layout, and the shaders whose reflection gives
// C++ its offsets and bindings.
class Pipelines {
public:
  Pipelines(VkDevice device, const Resources &resources);
  ~Pipelines();
  Pipelines(const Pipelines &) = delete;
  Pipelines &operator=(const Pipelines &) = delete;

  VkPipelineLayout layout() const;
  // Set 0, which every pass binds.
  VkDescriptorSet images() const;
  // The frame block's address, which every pass pushes (RV02); 0 until a shader declares
  // the block.
  VkDeviceAddress frame() const;
  // Before a frame runs: what the frame block holds (C01).
  void write_frame(std::uint64_t index,
                   double time,
                   VkExtent2D resolution,
                   std::array<float, 2> cursor) const;
  // The render pass a draw into an image of the format records in, made the first time
  // the format asks and kept, so every pipeline that draws into that format fits it.
  VkRenderPass render_pass(VkFormat format) const;

private:
  friend class Pipeline;
  friend class PassBlock;
  friend class Sampled;
  friend class Drawn;

  // The one frame block, laid out as the first shader that declares it says (RA03).
  struct Frame {
    Buffer buffer;
    std::vector<Field> fields;
  };

  // Throws naming the mismatch when the shader's block is not the one the engine writes,
  // so its node is refused (A02).
  void take_frame(const Shader &shader) const;

  // Pass blocks come from these; each block counts itself in and out of its pool.
  struct Pool {
    VkDescriptorPool handle = VK_NULL_HANDLE;
    std::uint32_t blocks = 0;
  };

  // A pool with room for a block, made when every pool is full, so a view's blocks are
  // bounded by memory alone.
  std::size_t pool() const;
  // Points a free slot of textures[] at the view; throws when none is free.
  std::uint32_t take_slot(VkImageView view) const;

  const VkDevice _device;
  const Resources &_resources;
  // In the order baseclasses/GpuLayout.glsl names them; set 0's layout holds them.
  std::array<VkSampler, static_samplers> _samplers{};
  VkDescriptorSetLayout _images = VK_NULL_HANDLE;
  VkDescriptorPool _image_pool = VK_NULL_HANDLE;
  VkDescriptorSet _image_set = VK_NULL_HANDLE;
  // textures[]'s free slots, the lowest last; slot 0, unbound, is never one.
  mutable std::vector<std::uint32_t> _free;
  VkDescriptorSetLayout _pass = VK_NULL_HANDLE;
  VkPipelineLayout _layout = VK_NULL_HANDLE;
  mutable std::vector<Pool> _pools;
  mutable std::optional<Frame> _frame;
  mutable std::vector<std::pair<VkFormat, VkRenderPass>> _render_passes;
};

// A dispatch's pipeline, or a draw's for a render pass.
class Pipeline {
public:
  Pipeline(const Pipelines &pipelines, const Shader &compute);
  Pipeline(const Pipelines &pipelines,
           const Shader &vertex,
           const Shader &fragment,
           VkRenderPass render_pass);
  Pipeline(Pipeline &&other) noexcept;
  Pipeline &operator=(Pipeline &&other) noexcept;
  ~Pipeline();

  VkPipeline handle() const;

private:
  VkDevice _device = VK_NULL_HANDLE;
  VkPipeline _pipeline = VK_NULL_HANDLE;
};

// One pass's block: the CPU writes it, the pass's shader reads it.
class PassBlock {
public:
  PassBlock(const Pipelines &pipelines, std::uint32_t size);
  PassBlock(PassBlock &&other) noexcept;
  PassBlock &operator=(PassBlock &&other) noexcept;
  ~PassBlock();

  VkDescriptorSet set() const;
  std::span<std::byte> bytes() const;
  void flush() const;

private:
  const Pipelines *_pipelines;
  Buffer _buffer;
  std::size_t _pool = 0; // of the pipelines' pools, the one the set came from
  VkDescriptorSet _set = VK_NULL_HANDLE;
};

// An image the passes sample, and its slot in textures[], which a pass block's Texture
// holds; the slot is free again once the image goes.
class Sampled {
public:
  Sampled(const Pipelines &pipelines, Image image);
  Sampled(Sampled &&other) noexcept;
  Sampled &operator=(Sampled &&other) noexcept;
  ~Sampled();

  const Image &image() const;
  std::uint32_t slot() const;

private:
  const Pipelines *_pipelines;
  Image _image;
  std::uint32_t _slot = 0; // 0 once moved from
};

// An image a draw renders into and passes then sample: its slot in textures[], and the
// framebuffer its draw records through, in its format's render pass.
class Drawn {
public:
  Drawn(const Pipelines &pipelines, Image image);
  Drawn(Drawn &&other) noexcept;
  Drawn &operator=(Drawn &&other) noexcept;
  ~Drawn();

  const Sampled &sampled() const;
  VkFramebuffer framebuffer() const;

private:
  Sampled _sampled;
  VkDevice _device = VK_NULL_HANDLE;
  VkFramebuffer _framebuffer = VK_NULL_HANDLE;
};

} // namespace VP
