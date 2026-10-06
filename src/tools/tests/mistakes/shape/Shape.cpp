#include "runtime/Operator.h"

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>

namespace {

constexpr std::uint32_t room = 8; // twice the invocations, as a draw's instances would

// The shader's Shape, member for member, as the loader checks it.
struct Shape {
  glm::vec2 at;
  float size = 0;
  std::uint32_t sides = 0;

  static constexpr auto members() {
    return std::array{
        VP_MEMBER(Shape, at), VP_MEMBER(Shape, size), VP_MEMBER(Shape, sides)};
  }
};

// The same members, with size and sides swapped: the loader must refuse it.
struct Swapped {
  glm::vec2 at;
  std::uint32_t sides = 0;
  float size = 0;

  static constexpr auto members() {
    return std::array{
        VP_MEMBER(Swapped, at), VP_MEMBER(Swapped, sides), VP_MEMBER(Swapped, size)};
  }
};

// The sum the shader makes of shape i, as the operator writes it.
float sum(std::size_t i) {
  return static_cast<float>(i) + 10.0f * 2.0f + 100.0f * 0.5f + 1000.0f * 3.0f;
}

// Stops when the shader read back other sums than the shapes' members make.
void check(std::span<const float> sums) {
  for (std::size_t i = 0; i < sums.size(); ++i)
    if (sums[i] != sum(i))
      throw std::runtime_error(
          std::format("shape {} summed to {}, not {}", i, sums[i], sum(i)));
}

// Writes a shape per invocation into a buffer with room for more, and stops when the
// shader read back other sums than its members make, a frame after it wrote them.
class Shapes final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _shapes = node.upload<Shape>("shapes", room);
    _sums = node.readback<float>("sums");
  }
  void cook(VP::Cook &frame) override {
    const std::span<Shape> shapes = frame.write(_shapes, room);
    for (std::size_t i = 0; i < shapes.size(); ++i)
      shapes[i] = {.at = {static_cast<float>(i), 2.0f}, .size = 0.5f, .sides = 3};
    check(frame.read(_sums));
  }

  VP::Upload<Shape> _shapes;
  VP::Readback<float> _sums;
};

// Reads back the sums of shapes another node's C++ wrote through a connection.
class Sums final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _sums = node.readback<float>("sums");
  }
  void cook(VP::Cook &frame) override {
    check(frame.read(_sums));
  }

  VP::Readback<float> _sums;
};

// Writes more shapes than its buffer holds, so its operator must stop.
class Overflow final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _shapes = node.upload<Shape>("shapes");
  }
  void cook(VP::Cook &frame) override {
    frame.write(_shapes, room);
  }

  VP::Upload<Shape> _shapes;
};

class Misplaced final : public VP::Operator {
  void bind(VP::Bind &node) override {
    node.upload<Swapped>("shapes");
    node.readback<float>("sums");
  }
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Shapes>("Shapes");
  registry.add<Sums>("Sums");
  registry.add<Misplaced>("Misplaced");
  registry.add<Overflow>("Overflow");
}
