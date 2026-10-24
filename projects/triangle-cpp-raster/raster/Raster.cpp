#include "runtime/Operator.h"

#include <glm/mat2x2.hpp>
#include <glm/mat3x3.hpp>
#include <glm/packing.hpp>
#include <glm/vector_relational.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>

namespace {

constexpr float turn = 2.0f * std::numbers::pi_v<float>; // radians in a whole turn

// The classic triangle: its corners in clip space, y pointing down, and a color each, a
// column of colors a corner.
constexpr std::array corners{
    glm::vec2(0.0f, -0.5f), glm::vec2(0.5f, 0.5f), glm::vec2(-0.5f, 0.5f)};
constexpr glm::mat3 colors(
    glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
constexpr glm::vec2 center = (corners[0] + corners[1] + corners[2]) / 3.0f;
constexpr std::uint32_t nothing = 0; // a packed color with no alpha: no cell at all

// How far the triangle has turned at a frame, counted from the frame index, so it never
// drifts; none while frames_per_turn is 0.
float angle_at(std::uint64_t frame, std::uint32_t frames_per_turn) {
  if (frames_per_turn == 0)
    return 0.0f;
  return turn * static_cast<float>(frame % frames_per_turn) /
         static_cast<float>(frames_per_turn);
}

// Twice the area of the triangle from a to b to p, its sign the side of a to b p is on.
float edge(glm::vec2 a, glm::vec2 b, glm::vec2 p) {
  return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

// Almost all on the CPU, as software rendering is: C++ turns the corners and fills every
// cell of the grid, line for line as the compute raster of triangle-compute does in GLSL,
// and packs each cell's color into four bytes, which the node's shader unpacks and places.
class Raster final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _packed = node.upload<std::uint32_t>("packed");
    _frames_per_turn = node.param<std::uint32_t>("frames_per_turn");
    _side = std::max(node.param<std::uint32_t>("side"), std::uint32_t{1});
  }
  void cook(VP::Cook &frame) override {
    const float angle = angle_at(frame.index(), _frames_per_turn);
    const glm::mat2 rotation(
        std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle));
    std::array<glm::vec2, corners.size()> at{};
    for (std::size_t corner = 0; corner < corners.size(); ++corner)
      at[corner] = center + rotation * (corners[corner] - center);
    const float area = edge(at[0], at[1], at[2]);
    const float size = 2.0f / static_cast<float>(_side);
    // One a cell, as many as the node's invocations; side * side of them are the grid.
    const std::span<std::uint32_t> packed = frame.write(_packed);
    for (std::size_t cell = 0; cell < packed.size(); ++cell) {
      const glm::vec2 corner = glm::vec2(cell % _side, cell / _side) * size - 1.0f;
      const glm::vec2 middle = corner + 0.5f * size;
      const glm::vec3 weights = glm::vec3(edge(at[1], at[2], middle),
                                          edge(at[2], at[0], middle),
                                          edge(at[0], at[1], middle)) /
                                area;
      const bool inside = glm::all(glm::greaterThanEqual(weights, glm::vec3(0.0f)));
      packed[cell] =
          inside ? glm::packUnorm4x8(glm::vec4(colors * weights, 1.0f)) : nothing;
    }
  }

  VP::Upload<std::uint32_t> _packed;
  std::uint32_t _frames_per_turn = 0;
  std::uint32_t _side = 1;
};

} // namespace

VP_OPERATOR(Raster);
