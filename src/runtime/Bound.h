#pragma once

#include "baseclasses/Pipelines.h"
#include "runtime/Operator.h"
#include "runtime/Text.h"
#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <map>
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

// The param's text as a T at out, which the field's offset gives, the T's components
// one after another, as std140 lays a vector; false when it does not parse.
template <class T> bool parse(std::string_view text, std::byte *out) {
  T value{};
  if (!parse_param(text, value))
    return false;
  if (out)
    std::memcpy(out, &value, sizeof value);
  return true;
}

// A param's text as the bytes of the field's GLSL type, of the first of Types the field
// is; false when it does not parse, or the field is none of them.
template <class... Types>
bool write_as(std::string_view text, std::string_view type, std::byte *out) {
  bool written = false;
  ((type == glsl_type<Types> && (written = parse<Types>(text, out), true)) || ...);
  return written;
}

// A param's text as the bytes of the field's GLSL type, a number or a vector of them;
// false when it does not parse.
inline bool write_value(std::string_view text, std::string_view type, std::byte *out) {
  return write_as<float,
                  std::int32_t,
                  std::uint32_t,
                  glm::vec2,
                  glm::vec3,
                  glm::vec4,
                  glm::ivec2,
                  glm::ivec3,
                  glm::ivec4,
                  glm::uvec2,
                  glm::uvec3,
                  glm::uvec4>(text, type, out);
}

inline bool reads(Access access) {
  return access != Access::write;
}

inline bool writes(Access access) {
  return access != Access::read;
}

// Where the build puts a shader's SPIR-V: beside it in the node's folder of the build tree.
inline std::filesystem::path spirv_of(const std::filesystem::path &folder,
                                      std::string_view shader) {
  return folder / (std::string(shader) + ".spv");
}

// A node runs the operator its operator word names, or with none the one its folder's C++
// registers, which Modules::sole finds.
inline bool runs_operator(const Node &node) {
  return !node.operator_name.empty() ||
         std::ranges::any_of(
             node.files, [](const std::string &file) { return file.ends_with(".cpp"); });
}

// A node whose folder holds a .vert draws, with its own .frag, or as a vertex node, with
// the fragment node its varyings reach, whose folder holds a lone .frag.
inline bool is_draw(const Node &node) {
  return holds_stage(node, vertex_stage);
}
inline bool is_vertex(const Node &node) {
  return is_draw(node) && !holds_stage(node, fragment_stage);
}
inline bool is_fragment(const Node &node) {
  return !is_draw(node) && holds_stage(node, fragment_stage);
}

// A draw's instances when its node gives them as a number, one when it gives none;
// nothing when the used length of a port's buffer counts them.
inline std::optional<std::uint32_t> instance_number(const Node &node) {
  if (node.instance_count.empty())
    return 1;
  return number_in<std::uint32_t>(node.instance_count);
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
// What an image a dispatch stores into takes: the one format images[] declares, rgba8.
inline constexpr std::string_view stored_format = "R8G8B8A8_UNORM";
// What a view's window renders into while the view hosting it takes it: sRGB, as the
// window prefers, so the view's draws blend and band as they would in a window of their
// own.
inline constexpr std::string_view window_format = "R8G8B8A8_SRGB";

inline const PixelFormat *pixel_format(std::string_view name) {
  const auto found = std::ranges::find(pixel_formats, name, &PixelFormat::name);
  return found == pixel_formats.end() ? nullptr : &*found;
}

// A buffer's element as C++ writes or reads it, described as reflection describes the
// shader's: its size as the stride, its GLSL type, or a struct's members.
inline Field element_field(std::string_view port, const Element &element) {
  Field field{.name = std::string(port),
              .type = std::string(element.type),
              .stride = static_cast<std::uint32_t>(element.size)};
  for (const Member &member : element.members)
    field.members.push_back({.name = std::string(member.name),
                             .type = std::string(member.type),
                             .offset = static_cast<std::uint32_t>(member.offset)});
  return field;
}

// Where C++ and a shader disagree on a buffer's element, each naming both sides; none
// when a struct agrees member for member, by name, type and offset (A02, RA03).
inline std::vector<std::string>
disagreements(const Field &cpp, const Field &glsl, std::string_view verb) {
  const bool members = !cpp.members.empty();
  if (cpp.stride != glsl.stride || (!members && cpp.type != glsl.type) ||
      members != !glsl.members.empty())
    return {
        std::format("the operator {} {} as {}-byte {}, but the shader holds {}-byte {}",
                    verb,
                    glsl.name,
                    cpp.stride,
                    members ? "structs" : cpp.type,
                    glsl.stride,
                    glsl.type)};
  std::vector<std::string> found;
  for (const Field &member : glsl.members) {
    const auto named = std::ranges::find(cpp.members, member.name, &Field::name);
    if (named == cpp.members.end())
      found.push_back(std::format(
          "{}: the shader's {} holds {}, which the C++ struct's members() does not name",
          glsl.name,
          glsl.type,
          member.name));
    else if (named->type != member.type || named->offset != member.offset)
      found.push_back(std::format(
          "{}: member {} is a {} at byte {} in C++, but a {} at byte {} in the shader",
          glsl.name,
          member.name,
          named->type,
          named->offset,
          member.type,
          member.offset));
  }
  for (const Field &member : cpp.members)
    if (std::ranges::find(glsl.members, member.name, &Field::name) == glsl.members.end())
      found.push_back(std::format("{}: the C++ struct names {}, which the shader's {} "
                                  "does not hold",
                                  glsl.name,
                                  member.name,
                                  glsl.type));
  return found;
}

// How far a smoothed time moves toward each frame's, so it follows over about 0.9 s at 60
// frames a second: slow enough to read, quick enough to show a change while one looks.
inline constexpr float smoothing = 1.0f / 54.0f;

// A frame's time, the last, and the smoothed one moved toward it, or set to it, the
// first time one is known.
inline void smooth(float &smoothed, float &last, bool &known, float took) {
  last = took;
  smoothed = known ? smoothed + (took - smoothed) * smoothing : took;
  known = true;
}

struct Bound {
  const Node *node = nullptr;
  Level log = Level::warn;
  std::vector<std::string> shaders_named;   // the node's shader files
  std::vector<std::filesystem::path> spirv; // by the node's shaders
  std::vector<std::filesystem::file_time_type> built;
  std::vector<Shader> shaders; // all of the node's, or none when one fails to load
  std::vector<Field> fields;   // the pass block its shaders share
  std::uint32_t block_size = 0;
  std::optional<Pipeline> pipeline;
  VkRenderPass render_pass = VK_NULL_HANDLE; // a draw's, which its pipeline fits
  // A draw's color, by the name its fragment shader gives it, and while a connection
  // takes it to a Texture, the format of the image it renders into; null for the window.
  // A fragment node holds its draw's, as the port is its own.
  std::string output;
  const PixelFormat *drawn = nullptr;
  // A vertex node's fragment node, with which it makes one draw; null for any other.
  Bound *fragment = nullptr;
  std::optional<PassBlock> block;
  std::unique_ptr<Operator> op;
  std::set<std::string, std::less<>> set_by_operator;
  // By name, the GLSL type its C++ reads the param as; empty for a word, or a param it
  // only asks whether the node sets.
  std::map<std::string, std::string, std::less<>> read_by_operator;
  std::vector<std::string> readbacks;           // ports, by Readback<T>::index
  std::vector<const Buffer *> readback_buffers; // the same, once the buffers exist
  // A buffer its C++ writes, by Upload<T>::index: its port, the elements C++ asked room
  // for, and once the buffers exist, the buffer and its used length.
  struct Written {
    std::string port;
    std::uint32_t count = 0; // 0 for one per invocation
    std::uint32_t stride = 0;
    Field element; // as C++ writes it, which a reader's shader must hold
    const Buffer *buffer = nullptr;
    std::uint32_t *used = nullptr;
  };
  std::vector<Written> uploads;
  // An image its C++ fills, by Texture::index: the port, the image's name, as the
  // writer's node.port, made once so asking whether it is empty allocates nothing a frame
  // (CPP10), and the format its node gives it, null when the node names one there is not.
  struct Filled {
    std::string port;
    std::string name;
    const PixelFormat *format = nullptr;
  };
  std::vector<Filled> textures;
  std::vector<Command> commands; // registered while it bound
  std::vector<File> files;       // opened while it bound
  std::vector<std::string> outputs; // its ports that carry C++ objects to C++ nodes
  std::vector<std::string> inputs;  // and that read them
  // Each input's type, as typeid names it, so a loose one finds the outputs alike.
  std::vector<std::string> input_types;
  // An object for each input nothing gives it, so its reference holds while it is in
  // error; made by its module, so it goes with it.
  std::vector<Object> stand_ins;
  std::vector<std::string> errors;
  // A hosted view's node of which a live run's build made nothing yet, as one of a project
  // loaded before it was ever built: left out as by an error, but only waiting, so it says
  // so quietly, as the build that makes it says why if it cannot (A02).
  bool unbuilt = false;
  // How many of its errors only wait for such a node, as a reader of what it writes: when
  // all of them, it says so quietly too.
  std::size_t waits = 0;
  // A dispatch's threads, as its word gives them or the buffer at its port counts them.
  std::uint32_t invocations = 0;
  // Its operator's bind ended in a throw, so what it would have asked for after is unknown.
  bool cut_short = false;
  // Its operator read the view as it bound, so the ports it asked for may follow the
  // connections, as a deck's rooms do, and only its next bind knows them.
  bool asked_view = false;

  bool loaded() const {
    return !shaders.empty();
  }
  // Whether its bind found every port and param it has: its operator's bind, if it runs
  // one, and its shaders, if it names any, ran through.
  bool whole() const {
    return !unbuilt && (!runs_operator(*node) || (op && !cut_short)) &&
           (shaders_named.empty() || loaded());
  }
  // Its shader of the stage, as reflection read it, which it must hold.
  const Shader &stage(std::string_view stage) const {
    const auto named = std::ranges::find_if(shaders_named, [&](const std::string &file) {
      return file.ends_with(stage);
    });
    return shaders.at(static_cast<std::size_t>(named - shaders_named.begin()));
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
    return node->param(key);
  }
};

// The object a connection between C++ nodes carries (native C++). Its writer's module
// makes and destroys it, so it goes when a swap rewrites that module (rule 2).
struct Held {
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
struct Picture {
  std::string name; // the writer's node.port
  Sampled sampled;
};

// A buffer holds one element per invocation of its writer: a dispatch's threads, or a
// draw's vertices.
inline std::uint32_t invocations_of(const Bound &bound) {
  return is_draw(*bound.node) ? bound.node->vertex_count : bound.invocations;
}

} // namespace VP
