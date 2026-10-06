#include "runtime/Operator.h"

#include <glm/gtc/packing.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>
#include <string_view>

namespace {

constexpr std::uint32_t width = 4;
constexpr std::uint32_t height = 2;
constexpr std::size_t count = std::size_t{width} * height;
// Row by row, each pixel apart from the others, so one the shader finds elsewhere than
// C++ put it reads as another.
constexpr std::array<std::uint8_t, count> bytes{0, 30, 60, 90, 120, 150, 180, 210};
constexpr float unorm = 255.0f; // the byte that reads as 1
// Values no byte holds, each a 16-bit float exactly.
constexpr std::array<float, count> deep{
    -1.5f, 0.25f, 1000.0f, 2048.0f, 0.125f, 3.5f, -7.0f, 60000.0f};

// A byte a pixel, R8_UNORM: filtering may round one, but never to its neighbour's value.
struct Bytes {
  using Pixel = std::uint8_t;
  static constexpr std::string_view port = "picture";
  static constexpr float tolerance = 0.5f / unorm;
  static std::array<Pixel, count> pixels() {
    return bytes;
  }
  static float expected(std::size_t i) {
    return static_cast<float>(bytes[i]) / unorm;
  }
};

// The same, through a port no Texture names, which a connection takes to one.
struct Relayed : Bytes {
  static constexpr std::string_view port = "copy";
};

// Four 16-bit floats a pixel, R16G16B16A16_SFLOAT, which the shader reads exactly.
struct Halves {
  using Pixel = glm::u16vec4;
  static constexpr std::string_view port = "picture";
  static constexpr float tolerance = 0.0f;
  static std::array<Pixel, count> pixels() {
    std::array<Pixel, count> halves{};
    for (std::size_t i = 0; i < count; ++i)
      halves[i] = Pixel(glm::packHalf1x16(deep[i]));
    return halves;
  }
  static float expected(std::size_t i) {
    return deep[i];
  }
};

// Fills its image once, and stops when the shader sampled another value at a pixel than
// C++ put there, a frame after. A frame after filling, it adds a node, which rebuilds
// the view: the image must stay through it, as the node fills it only once.
template <class Image> class Filler final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _image = node.texture<typename Image::Pixel>(Image::port);
    _texels = node.readback<float>("texels");
  }
  void cook(VP::Cook &frame) override {
    if (!_filled) {
      frame.upload(_image, Image::pixels(), {width, height});
      _filled = true;
    } else if (frame.index() == 1) {
      frame.commands().send("node add spare");
    }
    const std::span<const float> sampled = frame.read(_texels);
    for (std::size_t i = 0; i < sampled.size(); ++i)
      if (std::abs(sampled[i] - Image::expected(i)) > Image::tolerance)
        throw std::runtime_error(std::format("pixel {} sampled as {}, but C++ filled {}",
                                             i,
                                             sampled[i],
                                             Image::expected(i)));
  }

  VP::Texture<typename Image::Pixel> _image;
  VP::Readback<float> _texels;
  bool _filled = false;
};

// Fills an image nothing samples, in the format no word gives, and leaves its shader's
// Texture unfilled.
class Stray final : public VP::Operator {
  void bind(VP::Bind &node) override {
    node.texture<glm::u8vec4>("stray");
  }
};

// Uploads a pixel fewer than the size it gives.
class Misfilled final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _image = node.texture<std::uint8_t>("picture");
  }
  void cook(VP::Cook &frame) override {
    frame.upload(_image, std::span(bytes).first(count - 1), {width, height});
  }

  VP::Texture<std::uint8_t> _image;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Filler<Bytes>>("Picture");
  registry.add<Filler<Relayed>>("Relayed");
  registry.add<Filler<Halves>>("Deep");
  registry.add<Stray>("Stray");
  registry.add<Misfilled>("Misfilled");
}
