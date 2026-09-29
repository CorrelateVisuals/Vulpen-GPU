#pragma once

#include "baseclasses/Log.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>

// The one runtime header a recipe's C++ includes: a node's behaviour and the general
// ports (input, commands, files). Nothing here reaches Vulkan or the OS.
//
// A recipe gets names in and handles out, never an object that owns the GPU, so its
// module needs no engine symbol and links the same way into a release binary.

namespace VP {

// The GLSL name of each type C++ may put in a pass block; the loader compares it with
// the type the shader declares. glm's vectors are GLSL's, name for name.
template <class T> inline constexpr std::string_view glsl_type{};
template <> inline constexpr std::string_view glsl_type<float> = "float";
template <> inline constexpr std::string_view glsl_type<std::int32_t> = "int";
template <> inline constexpr std::string_view glsl_type<std::uint32_t> = "uint";
template <> inline constexpr std::string_view glsl_type<glm::vec2> = "vec2";
template <> inline constexpr std::string_view glsl_type<glm::vec3> = "vec3";
template <> inline constexpr std::string_view glsl_type<glm::vec4> = "vec4";
template <> inline constexpr std::string_view glsl_type<glm::ivec2> = "ivec2";
template <> inline constexpr std::string_view glsl_type<glm::ivec3> = "ivec3";
template <> inline constexpr std::string_view glsl_type<glm::ivec4> = "ivec4";
template <> inline constexpr std::string_view glsl_type<glm::uvec2> = "uvec2";
template <> inline constexpr std::string_view glsl_type<glm::uvec3> = "uvec3";
template <> inline constexpr std::string_view glsl_type<glm::uvec4> = "uvec4";

// A value in the node's pass block.
template <class T> struct Value {
  std::uint32_t offset = 0;
};

// One of the node's buffers, as the CPU sees it a frame after the GPU wrote it.
template <class T> struct Readback {
  std::uint32_t index = 0;
};

// One of the node's buffers, as the CPU writes it for the GPU to read that frame.
template <class T> struct Upload {
  std::uint32_t index = 0;
};

// What a node's C++ gets while it binds: names in, handles out. The loader checks each
// name against the node's shader and manifest entry before a frame runs (A02), so a
// frame looks nothing up.
class Bind {
public:
  template <class T> Value<T> value(std::string_view name) {
    static_assert(!glsl_type<T>.empty(),
                  "a pass block value is a float, int, uint or a glm vector of them");
    return {value_offset(name, glsl_type<T>)};
  }
  // An element glsl_type names is checked by type; any other by its size alone.
  template <class T> Readback<T> readback(std::string_view name) {
    static_assert(std::is_trivially_copyable_v<T>);
    return {readback_index(name, sizeof(T), glsl_type<T>)};
  }
  template <class T> Upload<T> upload(std::string_view name) {
    static_assert(std::is_trivially_copyable_v<T>);
    return {upload_index(name, sizeof(T), glsl_type<T>)};
  }
  template <class T> T param(std::string_view name) {
    const std::string_view text = param_text(name);
    T value{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size())
      param_invalid(name, glsl_type<T>);
    return value;
  }

protected:
  ~Bind() = default;

private:
  virtual std::uint32_t value_offset(std::string_view name, std::string_view type) = 0;
  virtual std::uint32_t readback_index(std::string_view name,
                                       std::size_t element_size,
                                       std::string_view type) = 0;
  virtual std::uint32_t upload_index(std::string_view name,
                                     std::size_t element_size,
                                     std::string_view type) = 0;
  virtual std::string_view param_text(std::string_view name) = 0;
  virtual void param_invalid(std::string_view name, std::string_view type) = 0;
};

// What a node's C++ gets every frame, before the GPU runs the node's pass.
class Cook {
public:
  template <class T> void set(Value<T> value, T to) {
    std::memcpy(block().data() + value.offset, &to, sizeof to);
  }
  // Empty until the GPU has written the buffer once.
  template <class T> std::span<const T> read(Readback<T> readback) const {
    const std::span<const std::byte> bytes = readback_bytes(readback.index);
    return {reinterpret_cast<const T *>(bytes.data()), bytes.size() / sizeof(T)};
  }
  // One element per invocation, zeroed when the buffer is made; what the CPU writes stays
  // until it writes again.
  template <class T> std::span<T> write(Upload<T> upload) {
    const std::span<std::byte> bytes = upload_bytes(upload.index);
    return {reinterpret_cast<T *>(bytes.data()), bytes.size() / sizeof(T)};
  }
  virtual std::uint64_t index() const = 0;
  virtual void log(Level level, std::string_view text) const = 0;

protected:
  ~Cook() = default;

private:
  virtual std::span<std::byte> block() = 0;
  virtual std::span<const std::byte> readback_bytes(std::uint32_t index) const = 0;
  virtual std::span<std::byte> upload_bytes(std::uint32_t index) = 0;
};

// A node's behaviour. State in its members lasts until its recipe's module is swapped;
// bind runs at load and again after every swap.
class Operator {
public:
  virtual ~Operator() = default;
  virtual void bind(Bind &) {}
  virtual void cook(Cook &) {}
};
class InputPort {};
class FilePort {};

// Where a recipe registers its operators, by the names a manifest's operator word uses.
class Registry {
public:
  using Make = std::unique_ptr<Operator> (*)();

  template <class T> void add(std::string_view name) {
    add(name, []() -> std::unique_ptr<Operator> { return std::make_unique<T>(); });
  }
  virtual void add(std::string_view name, Make make) = 0;

protected:
  ~Registry() = default;
};

} // namespace VP

// A recipe's one exported symbol. The build names and exports it, so the same source
// loads as a dev module or links into a release binary (docs/plans/live-code.md).
#define VP_RECIPE(registry)                                                              \
  extern "C" VP_RECIPE_EXPORT void VP_RECIPE_ENTRY(VP::Registry &registry)
