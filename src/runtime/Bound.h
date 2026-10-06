#pragma once

#include "baseclasses/Pipelines.h"
#include "runtime/Operator.h"
#include "runtime/Schedule.h"
#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// The schedule is three files, which share what this header holds and no other file
// includes: Schedule.cpp builds the view's GPU work, Binder.cpp binds and cooks each
// node's operator, and Checks.cpp holds what the loader refuses (A02).
namespace VP {

template <class T> bool parse(std::string_view text, std::byte *out) {
  T value{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    return false;
  if (out)
    std::memcpy(out, &value, sizeof value);
  return true;
}

// A param's text as the bytes of the field's GLSL type; false when it does not parse.
inline bool write_value(std::string_view text, std::string_view type, std::byte *out) {
  if (type == glsl_type<float>)
    return parse<float>(text, out);
  if (type == glsl_type<std::int32_t>)
    return parse<std::int32_t>(text, out);
  if (type == glsl_type<std::uint32_t>)
    return parse<std::uint32_t>(text, out);
  return false;
}

inline bool reads(Access access) {
  return access != Access::write;
}

inline bool writes(Access access) {
  return access != Access::read;
}

// The build compiles each shader as the stage its extension names, as glslang does, so
// the loader reads the stage from the same place. Every stage glslang names counts, so a
// node holding one Vulpen does not run yet is told so.
inline constexpr std::string_view compute_stage = ".comp";
inline constexpr std::string_view vertex_stage = ".vert";
inline constexpr std::string_view fragment_stage = ".frag";
inline constexpr std::array<std::string_view, 6> stages{
    compute_stage, vertex_stage, fragment_stage, ".geom", ".tesc", ".tese"};

inline bool is_draw(const Node &node) {
  return std::ranges::any_of(
      node.files, [](const std::string &file) { return file.ends_with(vertex_stage); });
}

// A draw's instances when its node gives them as a number, one when it gives none;
// nothing when the used length of a port's buffer counts them.
inline std::optional<std::uint32_t> instance_number(const Node &node) {
  if (node.instance_count.empty())
    return 1;
  std::uint32_t count = 0;
  const std::string &text = node.instance_count;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), count);
  if (error != std::errc{} || end != text.data() + text.size())
    return std::nullopt;
  return count;
}

// The formats an image may take (V04), by Vulkan's names (VK04), each with the pixel C++
// fills it with, byte for byte, as pixel_type names it.
struct PixelFormat {
  std::string_view name;
  VkFormat format = VK_FORMAT_UNDEFINED;
  std::string_view pixel;
  std::uint32_t bytes = 0;
};
inline constexpr std::array<PixelFormat, 10> pixel_formats{
    {{"R8_UNORM", VK_FORMAT_R8_UNORM, "uint8", 1},
     {"R8G8_UNORM", VK_FORMAT_R8G8_UNORM, "u8vec2", 2},
     {"R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM, "u8vec4", 4},
     {"R8G8B8A8_SRGB", VK_FORMAT_R8G8B8A8_SRGB, "u8vec4", 4},
     {"R16_SFLOAT", VK_FORMAT_R16_SFLOAT, "uint16", 2},
     {"R16G16_SFLOAT", VK_FORMAT_R16G16_SFLOAT, "u16vec2", 4},
     {"R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT, "u16vec4", 8},
     {"R32_SFLOAT", VK_FORMAT_R32_SFLOAT, "float", 4},
     {"R32G32_SFLOAT", VK_FORMAT_R32G32_SFLOAT, "vec2", 8},
     {"R32G32B32A32_SFLOAT", VK_FORMAT_R32G32B32A32_SFLOAT, "vec4", 16}}};
// What an image takes when its node gives it no format.
inline constexpr std::string_view default_format = "R8G8B8A8_UNORM";

inline const PixelFormat *pixel_format(std::string_view name) {
  const auto found = std::ranges::find(pixel_formats, name, &PixelFormat::name);
  return found == pixel_formats.end() ? nullptr : &*found;
}

inline std::string joined(const std::vector<std::string> &names) {
  std::string text;
  for (const std::string &name : names)
    text += (text.empty() ? "" : " and ") + name;
  return text;
}

struct Schedule::Bound {
  const Node *node = nullptr;
  Level log = Level::warn;
  std::vector<std::string> shaders_named;   // the node's shader files
  std::vector<std::filesystem::path> spirv; // by the node's shaders
  std::vector<std::filesystem::file_time_type> built;
  std::vector<Shader> shaders; // all of the node's, or none when one fails to load
  std::vector<Field> fields;   // the pass block its shaders share
  std::uint32_t block_size = 0;
  std::optional<Pipeline> pipeline;
  std::optional<PassBlock> block;
  std::unique_ptr<Operator> op;
  std::set<std::string, std::less<>> set_by_operator;
  std::set<std::string, std::less<>> read_by_operator;
  std::vector<std::string> readbacks;           // ports, by Readback<T>::index
  std::vector<const Buffer *> readback_buffers; // the same, once the buffers exist
  // A buffer its C++ writes, by Upload<T>::index: its port, the elements C++ asked room
  // for, and once the buffers exist, the buffer and its used length.
  struct Written {
    std::string port;
    std::uint32_t count = 0; // 0 for one per invocation
    std::uint32_t stride = 0;
    const Buffer *buffer = nullptr;
    std::uint32_t *used = nullptr;
  };
  std::vector<Written> uploads;
  // An image its C++ fills, by Texture::index: the port, and the format its node gives
  // it, null when the node names one there is not.
  struct Filled {
    std::string port;
    const PixelFormat *format = nullptr;
  };
  std::vector<Filled> textures;
  std::vector<Command> commands; // registered while it bound
  std::vector<File> files;       // opened while it bound
  std::vector<std::string> outputs; // its ports that carry C++ objects to C++ nodes
  std::vector<std::string> inputs;  // and that read them
  // An object for each input nothing gives it, so its reference holds while it is in
  // error; made by its module, so it goes with it.
  std::vector<Object> stand_ins;
  std::vector<std::string> errors;

  bool loaded() const {
    return !shaders.empty();
  }
  const Field *field(std::string_view name) const {
    const auto found = std::ranges::find(fields, name, &Field::name);
    return found == fields.end() ? nullptr : &*found;
  }
  const Written *upload(std::string_view port) const {
    const auto found = std::ranges::find(uploads, port, &Written::port);
    return found == uploads.end() ? nullptr : &*found;
  }
  bool uploads_to(std::string_view port) const {
    return upload(port) != nullptr;
  }
  bool fills(std::string_view port) const {
    return std::ranges::find(textures, port, &Filled::port) != textures.end();
  }
  const Param *param(std::string_view key) const {
    const auto found = std::ranges::find(node->params, key, &Param::key);
    return found == node->params.end() ? nullptr : &*found;
  }
};

// The object a connection between C++ nodes carries (native C++). Its writer's module
// makes and destroys it, so it goes when a swap rewrites that module (rule 2).
struct Schedule::Held {
  std::string name;   // the connection's, or the writer's node.port while unconnected
  std::string module; // the writer's, as "view/folder"
  std::string type;   // as typeid names it
  std::size_t size = 0;
  std::size_t align = 0;
  Object object;

  bool holds(const Kind &kind) const {
    return type == kind.type && size == kind.size && align == kind.align;
  }
};

// An image a node's C++ filled, by the port that fills it, so a connection made or
// removed keeps it, and so does a rebuild while the node keeps its name.
struct Schedule::Picture {
  std::string name; // the writer's node.port
  Sampled sampled;
};

} // namespace VP
