#include "runtime/Operator.h"

#include <array>
#include <cstdint>
#include <span>

namespace {

constexpr std::uint32_t count = 4; // one per invocation of the shape node that reads them

// The shader's Shape, as shape's own C++ names it too; the loader holds both to it.
struct Shape {
  glm::vec2 at;
  float size = 0;
  std::uint32_t sides = 0;

  static constexpr auto members() {
    return std::array{
        VP_MEMBER(Shape, at), VP_MEMBER(Shape, size), VP_MEMBER(Shape, sides)};
  }
};

// The same members, with size and sides swapped: the reader's shader must refuse it.
struct Swapped {
  glm::vec2 at;
  std::uint32_t sides = 0;
  float size = 0;

  static constexpr auto members() {
    return std::array{
        VP_MEMBER(Swapped, at), VP_MEMBER(Swapped, sides), VP_MEMBER(Swapped, size)};
  }
};

// Fills the shapes another node's shader sums, through a connection, as a C++ part with
// no shader fills a drawing part's buffer.
class Maker final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _shapes = node.upload<Shape>("shapes", count);
  }
  void cook(VP::Cook &frame) override {
    const std::span<Shape> shapes = frame.write(_shapes);
    for (std::size_t i = 0; i < shapes.size(); ++i)
      shapes[i] = {.at = {static_cast<float>(i), 2.0f}, .size = 0.5f, .sides = 3};
  }

  VP::Upload<Shape> _shapes;
};

class Mismade final : public VP::Operator {
  void bind(VP::Bind &node) override {
    node.upload<Swapped>("shapes", count);
  }
};

// Gives no room, which a buffer no shader of its node holds needs.
class Roomless final : public VP::Operator {
  void bind(VP::Bind &node) override {
    node.upload<Shape>("shapes");
  }
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Maker>("Maker");
  registry.add<Mismade>("Mismade");
  registry.add<Roomless>("Roomless");
}
