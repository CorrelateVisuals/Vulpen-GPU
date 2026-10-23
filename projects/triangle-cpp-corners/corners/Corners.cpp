#include "contracts/Corner.h"
#include "runtime/Operator.h"

#include <glm/mat2x2.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>

namespace {

constexpr float turn = 2.0f * std::numbers::pi_v<float>; // radians in a whole turn

// The classic triangle: its corners in clip space, y pointing down, and a color each.
constexpr std::array corners{
    glm::vec2(0.0f, -0.5f), glm::vec2(0.5f, 0.5f), glm::vec2(-0.5f, 0.5f)};
constexpr std::array colors{glm::vec4(1.0f, 0.0f, 0.0f, 1.0f),
                            glm::vec4(0.0f, 1.0f, 0.0f, 1.0f),
                            glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)};
constexpr glm::vec2 center = (corners[0] + corners[1] + corners[2]) / 3.0f;

// How far the triangle has turned at a frame, counted from the frame index, so it never
// drifts; none while frames_per_turn is 0.
float angle_at(std::uint64_t frame, std::uint32_t frames_per_turn) {
  if (frames_per_turn == 0)
    return 0.0f;
  return turn * static_cast<float>(frame % frames_per_turn) /
         static_cast<float>(frames_per_turn);
}

// The shape on the CPU, the pixels on the GPU: C++ turns the three corners and hands
// them, each with its color, to the raster, which fills the cells they cover. No shader
// of this node holds them, so it asks for room for each.
class Corners final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _corners = node.upload<VP_VIEW::Corner>("corners",
                                            static_cast<std::uint32_t>(corners.size()));
    _frames_per_turn = node.param<std::uint32_t>("frames_per_turn");
  }
  void cook(VP::Cook &frame) override {
    const float angle = angle_at(frame.index(), _frames_per_turn);
    const glm::mat2 rotation(
        std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle));
    const std::span<VP_VIEW::Corner> written = frame.write(_corners);
    for (std::size_t corner = 0; corner < corners.size(); ++corner) {
      const glm::vec2 at = center + rotation * (corners[corner] - center);
      written[corner] = {.position = glm::vec4(at, 0.0f, 1.0f), .color = colors[corner]};
    }
  }

  VP::Upload<VP_VIEW::Corner> _corners;
  std::uint32_t _frames_per_turn = 0;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Corners>("Corners");
}
