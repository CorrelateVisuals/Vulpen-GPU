#include "runtime/Operator.h"

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>

namespace {

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

// Writes a shape per invocation and stops when the shader read back other sums than its
// members make, a frame after it wrote them.
class Shapes final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _shapes = node.upload<Shape>("shapes");
    _sums = node.readback<float>("sums");
  }
  void cook(VP::Cook &frame) override {
    const std::span<Shape> shapes = frame.write(_shapes);
    for (std::size_t i = 0; i < shapes.size(); ++i)
      shapes[i] = {.at = {static_cast<float>(i), 2.0f}, .size = 0.5f, .sides = 3};
    const std::span<const float> sums = frame.read(_sums);
    for (std::size_t i = 0; i < sums.size(); ++i)
      if (sums[i] != sum(i))
        throw std::runtime_error(
            std::format("shape {} summed to {}, not {}", i, sums[i], sum(i)));
  }

  VP::Upload<Shape> _shapes;
  VP::Readback<float> _sums;
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
  registry.add<Misplaced>("Misplaced");
}
