#include "runtime/Operator.h"
#include "shared/Triangle.h"

#include <cstdint>
#include <iterator>
#include <span>

namespace {

// C++ turns the corners with shared/Triangle.h, as a shader could with the same lines,
// and hands them to the vertex shader, which colors them from the same file.
class Triangle final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _points = node.upload<glm::vec2>("points");
    _frames_per_turn = node.param<std::uint32_t>("frames_per_turn");
  }
  void cook(VP::Cook &frame) override {
    // Counted in 32 bits, as GLSL counts the frame index, so the two turn alike.
    const float angle = angle_at(static_cast<uint>(frame.index()), _frames_per_turn);
    // One a vertex: a vertex_count past 3 draws the triangle again.
    const std::span<glm::vec2> points = frame.write(_points);
    for (std::size_t vertex = 0; vertex < points.size(); ++vertex)
      points[vertex] = corner_at(static_cast<int>(vertex % std::size(corners)), angle);
  }

  VP::Upload<glm::vec2> _points;
  std::uint32_t _frames_per_turn = 0;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Triangle>("Triangle");
}
