#include "runtime/Operator.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>

namespace {

constexpr std::uint32_t width = 4;
constexpr std::uint32_t height = 2;
// Row by row, each pixel apart from the others, so one the shader finds elsewhere than
// C++ put it reads as another.
constexpr std::array<std::uint8_t, width * height> pixels{
    0, 30, 60, 90, 120, 150, 180, 210};
constexpr float unorm = 255.0f; // the byte that reads as 1
// Filtering may round a pixel, but never to its neighbour's value.
constexpr float tolerance = 0.5f / unorm;

// Fills the image once, and stops when the shader sampled other pixels than C++ put
// there, a frame after. A frame after filling, it adds a node, which rebuilds the view:
// the image must stay through it, or the shader samples nothing from then on.
void fill_and_check(VP::Cook &frame,
                    VP::Texture image,
                    VP::Readback<float> texels,
                    bool &filled) {
  if (!filled) {
    frame.upload(image, pixels, {width, height});
    filled = true;
  } else if (frame.index() == 1) {
    frame.commands().send("node add spare");
  }
  const std::span<const float> sampled = frame.read(texels);
  for (std::size_t i = 0; i < sampled.size(); ++i)
    if (std::abs(sampled[i] - static_cast<float>(pixels[i]) / unorm) > tolerance)
      throw std::runtime_error(std::format(
          "pixel {} sampled as {}, but C++ filled {}", i, sampled[i], pixels[i]));
}

// Fills the image its own Texture samples.
class Picture final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _image = node.texture("picture");
    _texels = node.readback<float>("texels");
  }
  void cook(VP::Cook &frame) override {
    fill_and_check(frame, _image, _texels, _filled);
  }

  VP::Texture _image;
  VP::Readback<float> _texels;
  bool _filled = false;
};

// Fills an image of a port no Texture names, which a connection takes to one.
class Relayed final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _image = node.texture("copy");
    _texels = node.readback<float>("texels");
  }
  void cook(VP::Cook &frame) override {
    fill_and_check(frame, _image, _texels, _filled);
  }

  VP::Texture _image;
  VP::Readback<float> _texels;
  bool _filled = false;
};

// Fills an image nothing samples, and leaves its shader's Texture unfilled.
class Stray final : public VP::Operator {
  void bind(VP::Bind &node) override {
    node.texture("stray");
  }
};

// Uploads a pixel fewer than the size it gives.
class Misfilled final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _image = node.texture("picture");
  }
  void cook(VP::Cook &frame) override {
    frame.upload(_image, std::span(pixels).first(pixels.size() - 1), {width, height});
  }

  VP::Texture _image;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Picture>("Picture");
  registry.add<Relayed>("Relayed");
  registry.add<Stray>("Stray");
  registry.add<Misfilled>("Misfilled");
}
