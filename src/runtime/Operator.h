#pragma once

#include "baseclasses/Log.h"
#include "runtime/Commands.h"

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
// the type the shader declares.
template <class T> inline constexpr std::string_view glsl_type{};
template <> inline constexpr std::string_view glsl_type<float> = "float";
template <> inline constexpr std::string_view glsl_type<std::int32_t> = "int";
template <> inline constexpr std::string_view glsl_type<std::uint32_t> = "uint";

// A value in the node's pass block.
template <class T> struct Value {
  std::uint32_t offset = 0;
};

// One of the node's buffers, as the CPU sees it a frame after the GPU wrote it.
template <class T> struct Readback {
  std::uint32_t index = 0;
};

// What a node's C++ gets while it binds: names in, handles out. The loader checks each
// name against the node's shader and manifest entry before a frame runs (A02), so a
// frame looks nothing up.
class Bind {
public:
  template <class T> Value<T> value(std::string_view name) {
    static_assert(!glsl_type<T>.empty(), "a pass block value is a float, int or uint");
    return {value_offset(name, glsl_type<T>)};
  }
  // A float, int or uint element is checked by type; any other by its size alone.
  template <class T> Readback<T> readback(std::string_view name) {
    static_assert(std::is_trivially_copyable_v<T>);
    return {readback_index(name, sizeof(T), glsl_type<T>)};
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
  template <class T> std::span<const T> read(Readback<T> readback) {
    const std::span<const std::byte> bytes = readback_bytes(readback.index);
    return {reinterpret_cast<const T *>(bytes.data()), bytes.size() / sizeof(T)};
  }
  virtual std::uint64_t index() const = 0;
  virtual void log(Level level, std::string_view text) const = 0;

protected:
  ~Cook() = default;

private:
  virtual std::span<std::byte> block() = 0;
  virtual std::span<const std::byte> readback_bytes(std::uint32_t index) = 0;
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
