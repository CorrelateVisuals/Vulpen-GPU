#include "contracts/Corner.h"
#include "contracts/Spin.h"
#include "runtime/Operator.h"

#include <glm/mat2x2.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <span>

namespace {

// The classic triangle: its corners in clip space, y pointing down, and a color each.
constexpr std::array corners{
    glm::vec2(0.0f, -0.5f), glm::vec2(0.5f, 0.5f), glm::vec2(-0.5f, 0.5f)};
constexpr std::array colors{glm::vec4(1.0f, 0.0f, 0.0f, 1.0f),
                            glm::vec4(0.0f, 1.0f, 0.0f, 1.0f),
                            glm::vec4(0.0f, 0.0f, 1.0f, 1.0f)};
constexpr glm::vec2 center = (corners[0] + corners[1] + corners[2]) / 3.0f;

// Turns the corners by the clock's spin and writes them, each with its color, for the
// face to draw. No shader of this node holds them, so it asks for room for each.
class Corners final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _spin = &node.input<VP_VIEW::Spin>("spin");
    _corners = node.upload<VP_VIEW::Corner>("corners",
                                            static_cast<std::uint32_t>(corners.size()));
  }
  void cook(VP::Cook &frame) override {
    const float angle = _spin->angle;
    const glm::mat2 rotation(
        std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle));
    const std::span<VP_VIEW::Corner> written = frame.write(_corners);
    for (std::size_t corner = 0; corner < corners.size(); ++corner) {
      const glm::vec2 at = center + rotation * (corners[corner] - center);
      written[corner] = {.position = glm::vec4(at, 0.0f, 1.0f), .color = colors[corner]};
    }
  }

  const VP_VIEW::Spin *_spin{};
  VP::Upload<VP_VIEW::Corner> _corners;
};

} // namespace

VP_OPERATOR(Corners);
