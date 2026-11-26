#pragma once

// Static, so each part that includes this keeps stb_image to itself, and copies in two
// views link into one release binary (RV09); the formats a picture file takes, 8 bits a
// channel, read from the bytes the file port reads, so it holds no code for the others,
// and none that opens a file.
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#define STBI_ONLY_GIF
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
// A release build inlines stb_image into decoded(), past the system header that keeps its
// warnings out (RA02), and GCC then cannot see that bytes in memory never take the
// branch that reads from a callback, whose fields they leave unset.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include <stb_image.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <glm/ext/vector_uint4_sized.hpp>
#include <glm/vec2.hpp>

#include <cstddef>
#include <format>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

// "The same came therefore to Philip, which was of Bethsaida of Galilee, and desired him,
// saying, Sir, we would see Jesus." John 12:21

// A picture file's pixels, 8-bit RGBA row by row from the top left, as frame.upload takes
// them, and how many each way.
struct Pixels {
  std::vector<glm::u8vec4> rgba;
  glm::uvec2 size{};
};

// The bytes of a PNG, a JPEG, a BMP, a TGA or a GIF's first frame, decoded; throws
// naming the file and why stb_image read none. A node's file port reads a file not there
// as no bytes, so those are named as either, not as no image.
inline Pixels decoded(std::string_view bytes, std::string_view file) {
  if (bytes.empty())
    throw std::runtime_error(std::format("{} is empty, or not there", file));
  constexpr int rgba = 4; // channels a pixel takes
  glm::ivec2 size{};
  int channels = 0;
  const std::unique_ptr<stbi_uc, void (*)(void *)> pixels(
      stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(bytes.data()),
                            static_cast<int>(bytes.size()),
                            &size.x,
                            &size.y,
                            &channels,
                            rgba),
      stbi_image_free);
  if (!pixels)
    throw std::runtime_error(
        std::format("{} is no image stb_image reads: {}", file, stbi_failure_reason()));
  const auto *first = reinterpret_cast<const glm::u8vec4 *>(pixels.get());
  const auto count = static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y);
  return {{first, first + count}, glm::uvec2(size)};
}

} // namespace
