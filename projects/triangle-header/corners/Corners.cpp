#include "Corners.h"

#include <glm/mat2x2.hpp>

#include <array>
#include <cmath>
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

// No shader of this node holds corners, so the node asks for room for each corner, and
// the connection takes them to the face's.
void Corners::bind(VP::Bind &node) {
  _corners =
      node.upload<VP_VIEW::Corner>("corners", static_cast<std::uint32_t>(corners.size()));
  _frames_per_turn = node.param<std::uint32_t>("frames_per_turn");
}

void Corners::cook(VP::Cook &frame) {
  const float angle = angle_at(frame.index(), _frames_per_turn);
  const glm::mat2 rotation(
      std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle));
  const std::span<VP_VIEW::Corner> written = frame.write(_corners);
  for (std::size_t corner = 0; corner < corners.size(); ++corner) {
    const glm::vec2 at = center + rotation * (corners[corner] - center);
    written[corner] = {.position = glm::vec4(at, 0.0f, 1.0f), .color = colors[corner]};
  }
}

} // namespace

VP_OPERATORS(registry) {
  registry.add<Corners>("Corners");
}
