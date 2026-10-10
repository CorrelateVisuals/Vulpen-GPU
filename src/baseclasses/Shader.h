#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VP {

// Where a pass's own values and buffers live (RV02); every shader declares its pass
// block there, and C++ binds it there.
inline constexpr std::uint32_t pass_set = 1;
inline constexpr std::uint32_t pass_binding = 0;
// The struct a pass block names an image by, as baseclasses/GpuLayout.glsl declares it.
inline constexpr std::string_view texture_type = "Texture";

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
  bool texture() const {
    return type == texture_type;
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
  // What it writes out of its stage, by name: a fragment shader's colors, each a port of
  // its node. A built-in, as gl_Position, is none.
  const std::vector<std::string> &outputs() const;

private:
  std::vector<std::uint32_t> _words;
  std::vector<Field> _fields;
  std::uint32_t _block_size = 0;
  std::vector<Field> _frame;
  std::uint32_t _frame_size = 0;
  std::array<std::uint32_t, 3> _workgroup_size{};
  std::vector<std::string> _outputs;
};

} // namespace VP
