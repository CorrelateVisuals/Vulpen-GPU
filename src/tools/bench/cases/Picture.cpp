#include "shared/Picture.h"

#include "tools/bench/Bench.h"

// As the snapshot part writes its PNGs: static and into memory, here to make each
// format's bytes for decoded to read.
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's shared/Picture.h: a picture file's bytes decoded to 8-bit RGBA, as the
// picture and image-file parts upload them. The files are written here by stb_image_write
// from a picture of 3 by 2 pixels: red, green, blue over white, half see-through gray,
// black.

namespace VP::bench {

template <> struct Shown<::Pixels> {
  static std::string text(const ::Pixels &pixels, Names &names) {
    return std::format(
        "{} pixels {}", show(pixels.size, names), show(pixels.rgba, names));
  }
};

namespace {

constexpr int wide = 3;
constexpr int high = 2;
constexpr int rgba = 4;

const std::vector<std::uint8_t> &picture() {
  static const std::vector<std::uint8_t> pixels{255, 0,   0,   255, 0,   255, 0,   255,
                                                0,   0,   255, 255, 255, 255, 255, 255,
                                                128, 128, 128, 128, 0,   0,   0,   255};
  return pixels;
}

void append(void *into, void *data, int size) {
  static_cast<std::string *>(into)->append(static_cast<const char *>(data),
                                           static_cast<std::size_t>(size));
}

// Each format the part takes, as a file holds it.
std::vector<std::pair<std::string, std::string>> files() {
  std::vector<std::pair<std::string, std::string>> made;
  const auto write = [&](std::string name, auto &&writer) {
    std::string bytes;
    writer(bytes);
    made.emplace_back(std::move(name), std::move(bytes));
  };
  const void *const pixels = picture().data();
  write("a.png", [&](std::string &bytes) {
    stbi_write_png_to_func(append, &bytes, wide, high, rgba, pixels, wide * rgba);
  });
  write("a.bmp", [&](std::string &bytes) {
    stbi_write_bmp_to_func(append, &bytes, wide, high, rgba, pixels);
  });
  write("a.tga", [&](std::string &bytes) {
    stbi_write_tga_to_func(append, &bytes, wide, high, rgba, pixels);
  });
  write("a.jpg", [&](std::string &bytes) {
    stbi_write_jpg_to_func(append, &bytes, wide, high, rgba, pixels, 100);
  });
  const std::string png = made.front().second;
  made.emplace_back("cut.png", png.substr(0, png.size() / 2));
  made.emplace_back("empty.png", "");
  made.emplace_back("text.png", "not a picture");
  // A PNG whose header says it is wider than stb_image reads, 2^24 pixels.
  std::string wider = png;
  wider[16] = '\x01';
  made.emplace_back("wider.png", wider);
  return made;
}

} // namespace

void cases(Bench &bench) {
  bench.check("Picture.h/decoded", [](Rows &rows) {
    for (const auto &[file, bytes] : files()) {
      std::string said;
      try {
        said = show(decoded(bytes, file), rows.names());
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("{} of {} bytes -> {}", file, bytes.size(), said));
    }
  });
  bench.sweep("Picture.h/decoded", decoded, domain::Type{}, domain::Name{});
}

} // namespace VP::bench
