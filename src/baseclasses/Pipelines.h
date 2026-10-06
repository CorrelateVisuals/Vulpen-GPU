#pragma once

#include "baseclasses/Resources.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace VP {

// Where a pass's own values and buffers live (RV02); every shader declares its pass
// block there, and C++ binds it there.
inline constexpr std::uint32_t pass_set = 1;
inline constexpr std::uint32_t pass_binding = 0;

// What a shader does with a buffer, read from its qualifier: the node's declaration of
// what it reads and writes, from which the barriers follow (V10).
enum class Access { read, write, read_write };

struct Field {
  std::string name;
  std::string type;         // as GLSL names it; for a buffer, the type of its elements
  std::uint32_t offset = 0; // bytes from the start of the pass block
  std::uint32_t stride = 0; // bytes per element, only for a buffer
  Access access = Access::read;
  // For a buffer of structs, each member by name, GLSL type and offset in the element.
  std::vector<Field> members;

  bool buffer() const {
    return stride != 0;
  }
  bool operator==(const Field &) const = default;
};

// Compiled GLSL, reflected when it loads: C++ takes the pass block's offsets from here,
// never from a copy of its own (RA03).
class Shader {
public:
  explicit Shader(const std::filesystem::path &spirv);

  std::span<const std::uint32_t> words() const;
  // Empty when the shader declares no pass block.
  const std::vector<Field> &fields() const;
  std::uint32_t block_size() const;
  // The frame block the push constant points to (RV02); empty when it declares none.
  const std::vector<Field> &frame() const;
  std::uint32_t frame_size() const;
  // Zero for any stage but compute.
  const std::array<std::uint32_t, 3> &workgroup_size() const;

private:
  std::vector<std::uint32_t> _words;
  std::vector<Field> _fields;
  std::uint32_t _block_size = 0;
  std::vector<Field> _frame;
  std::uint32_t _frame_size = 0;
  std::array<std::uint32_t, 3> _workgroup_size{};
};

// What the GPU runs: the one pipeline layout, and the shaders whose reflection gives
// C++ its offsets and bindings.
class Pipelines {
public:
  Pipelines(VkDevice device, const Resources &resources);
  ~Pipelines();
  Pipelines(const Pipelines &) = delete;
  Pipelines &operator=(const Pipelines &) = delete;

  VkPipelineLayout layout() const;
  // The frame block's address, which every pass pushes (RV02); 0 until a shader declares
  // the block.
  VkDeviceAddress frame() const;
  // Before a frame runs: what the frame block holds (C01).
  void write_frame(std::uint64_t index, double time, VkExtent2D resolution) const;

private:
  friend class Pipeline;
  friend class PassBlock;

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

  const VkDevice _device;
  const Resources &_resources;
  VkDescriptorSetLayout _images = VK_NULL_HANDLE; // set 0; gains its arrays with a user
  VkDescriptorSetLayout _pass = VK_NULL_HANDLE;
  VkPipelineLayout _layout = VK_NULL_HANDLE;
  mutable std::vector<Pool> _pools;
  mutable std::optional<Frame> _frame;
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

} // namespace VP
