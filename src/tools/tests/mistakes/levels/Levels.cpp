#include "runtime/Operator.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <stdexcept>

namespace {

constexpr std::uint32_t side = 4;
constexpr std::size_t below = 4; // level 1's texels, then level 2's one
constexpr double unorm = 255.0;  // the byte that reads as 1
// Within a step of an 8-bit sRGB value at its brightest, as Vulpen rounds a level's
// color to the byte nearest in light, which may be a step from the nearest in code.
constexpr float tolerance = 0.01f;

// Row by row, R8G8B8A8_SRGB, in 2 by 2 blocks that tell apart how a level averages:
// black and white, which in light average to half and in code far darker; red beside
// three clear whites, which lend it no color; four grays; four clear colors, which, with
// nothing covered, average plainly.
using Pixel = glm::u8vec4;
constexpr Pixel black{0, 0, 0, 255}, white{255, 255, 255, 255}, red{255, 0, 0, 255},
    clear{255, 255, 255, 0}, dark{64, 64, 64, 255}, gray{128, 128, 128, 255},
    clear_red{255, 0, 0, 0}, clear_green{0, 255, 0, 0}, clear_blue{0, 0, 255, 0};
constexpr std::array<Pixel, std::size_t{side} * side> pixels{black,
                                                             white,
                                                             red,
                                                             clear,
                                                             white,
                                                             black,
                                                             clear,
                                                             clear,
                                                             black,
                                                             dark,
                                                             clear_red,
                                                             clear_green,
                                                             gray,
                                                             white,
                                                             clear_blue,
                                                             clear};

// sRGB's transfer (IEC 61966-2-1).
constexpr double toe = 0.04045;
constexpr double toe_light = 0.0031308;
constexpr double toe_slope = 12.92;
constexpr double offset = 0.055;
constexpr double srgb_gamma = 2.4;

double decoded(double code) {
  return code <= toe ? code / toe_slope
                     : std::pow((code + offset) / (1.0 + offset), srgb_gamma);
}

double encoded(double light) {
  return light <= toe_light ? light * toe_slope
                            : (1.0 + offset) * std::pow(light, 1.0 / srgb_gamma) - offset;
}

// A texel as the GPU samples it: its color in light, its alpha as is.
glm::dvec4 sampled(Pixel texel) {
  return {decoded(texel.r / unorm),
          decoded(texel.g / unorm),
          decoded(texel.b / unorm),
          texel.a / unorm};
}

// The texel over four, each color weighed by its alpha, plainly with nothing covered,
// then stored as 8-bit sRGB: worked out apart from Vulpen's integers, with the
// library's pow.
Pixel level(const std::array<Pixel, 4> &under) {
  glm::dvec4 weighed{0.0}, plain{0.0};
  double weight = 0.0;
  for (const Pixel texel : under) {
    const glm::dvec4 light = sampled(texel);
    weighed += light * light.a;
    plain += light;
    weight += light.a;
  }
  const double texels = static_cast<double>(under.size());
  const glm::dvec3 color =
      weight > 0.0 ? glm::dvec3(weighed) / weight : glm::dvec3(plain) / texels;
  const auto code = [](double value) {
    return static_cast<std::uint8_t>(std::lround(value * unorm));
  };
  return {code(encoded(color.r)),
          code(encoded(color.g)),
          code(encoded(color.b)),
          code(plain.a / texels)};
}

std::array<Pixel, below + 1> expected() {
  std::array<Pixel, below + 1> levels{};
  for (std::uint32_t i = 0; i < below; ++i) {
    const std::uint32_t x = (i % 2) * 2, y = (i / 2) * 2;
    levels[i] = level({pixels[y * side + x],
                       pixels[y * side + x + 1],
                       pixels[(y + 1) * side + x],
                       pixels[(y + 1) * side + x + 1]});
  }
  levels[below] = level({levels[0], levels[1], levels[2], levels[3]});
  return levels;
}

// Fills its image once with every mip level, and stops when the shader sampled a level's
// texel other than the averages above, a frame after.
class Levels final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _image = node.texture<Pixel>("picture");
    _texels = node.readback<glm::vec4>("texels");
  }
  void cook(VP::Cook &frame) override {
    if (!_filled) {
      frame.upload(_image, pixels, {side, side}, VP::all_mip_levels);
      _filled = true;
    }
    const std::span<const glm::vec4> read = frame.read(_texels);
    const std::array<Pixel, below + 1> levels = expected();
    for (std::size_t i = 0; i < read.size() && i < levels.size(); ++i) {
      const glm::dvec4 wanted = sampled(levels[i]);
      for (glm::length_t c = 0; c < glm::dvec4::length(); ++c)
        if (std::abs(read[i][c] - static_cast<float>(wanted[c])) > tolerance)
          throw std::runtime_error(
              std::format("mip texel {} channel {} sampled as {}, but averages to {}",
                          i,
                          c,
                          read[i][c],
                          wanted[c]));
    }
  }

  VP::Texture<Pixel> _image;
  VP::Readback<glm::vec4> _texels;
  bool _filled = false;
};

} // namespace

VP_OPERATOR(Levels);
