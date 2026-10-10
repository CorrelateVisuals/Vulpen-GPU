#include "runtime/Operator.h"

#include <cmath>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>

namespace {

constexpr float unorm = 255.0f;               // the byte that reads as 1
constexpr float tolerance = 0.5f / unorm;     // less than a byte, so a neighbour differs
constexpr std::uint64_t first_read = 2;       // a readback lags a frame behind its pass
constexpr glm::uvec2 stride{37, 23};          // between the pixels, as Look.comp steps
constexpr std::uint32_t byte_mask = 255;

// Stops when a pixel paint drew reads back as another, or when nothing reads back: the
// image a draw renders into must reach a dispatch after it in the same frame, at the
// frame's size, which --size gives without a window (V07).
class Look final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _texels = node.readback<glm::vec2>("texels");
  }
  void cook(VP::Cook &frame) override {
    const std::span<const glm::vec2> texels = frame.read(_texels);
    if (texels.empty() && frame.index() >= first_read)
      throw std::runtime_error("nothing read back: look's dispatch never ran");
    const glm::uvec2 size = frame.resolution();
    if (size.x == 0 || size.y == 0)
      throw std::runtime_error("no size to draw at: a run with no window takes --size");
    for (std::uint32_t i = 0; i < texels.size(); ++i) {
      const glm::uvec2 pixel = glm::uvec2(i) * stride % size;
      const glm::vec2 drawn = glm::vec2(pixel & byte_mask) / unorm;
      if (std::abs(texels[i].x - drawn.x) > tolerance ||
          std::abs(texels[i].y - drawn.y) > tolerance)
        throw std::runtime_error(std::format("pixel {} {} read back as {} {}, but paint "
                                             "drew {} {}",
                                             pixel.x,
                                             pixel.y,
                                             texels[i].x,
                                             texels[i].y,
                                             drawn.x,
                                             drawn.y));
    }
  }

  VP::Readback<glm::vec2> _texels;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Look>("Look");
}
