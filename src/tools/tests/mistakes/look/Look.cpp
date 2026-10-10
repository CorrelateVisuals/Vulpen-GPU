#include "runtime/Operator.h"

#include <cmath>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {

constexpr float unorm = 255.0f;               // the byte that reads as 1
constexpr float tolerance = 0.5f / unorm;     // less than a byte, so a neighbour differs
constexpr std::uint64_t first_read = 2;       // a readback lags a frame behind its pass
constexpr glm::uvec2 stride{37, 23};          // between the pixels, as Look.comp steps
constexpr std::uint32_t byte_mask = 255;
constexpr std::uint32_t block_shift = 3;      // scene's blocks are 8 pixels wide

// Stops when a pixel paint drew reads back as another, or when nothing reads back: the
// image a draw renders into must reach a dispatch after it in the same frame, at the
// frame's size, which --size gives without a window (V07).
class Look : public VP::Operator {
public:
  void bind(VP::Bind &node) override {
    _texels = node.readback<glm::vec2>("texels");
  }
  void cook(VP::Cook &frame) override {
    const std::span<const glm::vec2> texels = frame.read(_texels);
    if (texels.empty()) {
      if (frame.index() >= first_read)
        throw std::runtime_error("nothing read back: look's dispatch never ran");
      return;
    }
    const glm::uvec2 size = frame.resolution();
    if (size.x == 0 || size.y == 0)
      throw std::runtime_error("no size to draw at: a run with no window takes --size");
    for (std::uint32_t i = 0; i < texels.size(); ++i) {
      const glm::uvec2 pixel = glm::uvec2(i) * stride % size;
      const glm::vec2 drawn = drawn_at(pixel, frame.index() - 1);
      if (std::abs(texels[i].x - drawn.x) > tolerance ||
          std::abs(texels[i].y - drawn.y) > tolerance)
        throw std::runtime_error(std::format("pixel {} {} read back as {} {}, but {} "
                                             "drew {} {}",
                                             pixel.x,
                                             pixel.y,
                                             texels[i].x,
                                             texels[i].y,
                                             drawer(),
                                             drawn.x,
                                             drawn.y));
    }
  }

private:
  // Each pixel's place, the same every frame, as bytes R8G8B8A8_UNORM holds exactly.
  virtual glm::vec2 drawn_at(glm::uvec2 pixel, std::uint64_t) const {
    return glm::vec2(pixel & byte_mask) / unorm;
  }
  virtual std::string_view drawer() const {
    return "paint";
  }

  VP::Readback<glm::vec2> _texels;
};

// The same, for the window of the view look's view hosts, which scene draws into: it must
// be drawn before the dispatch of the view hosting it samples it, the same frame, so a
// pixel that reads back as the frame before's stops it.
class Watch final : public Look {
  glm::vec2 drawn_at(glm::uvec2 pixel, std::uint64_t frame) const override {
    const glm::uvec2 block = pixel >> block_shift;
    return {static_cast<float>((block.x + frame) & 1u), static_cast<float>(block.y & 1u)};
  }
  std::string_view drawer() const override {
    return "scene";
  }
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Look>("Look");
  registry.add<Watch>("Watch");
}
