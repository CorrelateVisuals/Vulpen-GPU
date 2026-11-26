#include "picture/Picture.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Worlds.h"

// As the snapshot part writes its PNGs: static and into memory, here to make a file the
// part decodes.
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/picture: the picture file its file param names, decoded into its
// texture with the mip levels asked, decoded again once the file's version moves; a file
// it cannot read or decode is named in the log, and the texture keeps what it held.

namespace VP::bench {

namespace domain {
// The mip_levels param as a manifest gives it.
struct LevelsWord {};
} // namespace domain

template <> struct Gen<domain::LevelsWord> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {
        "all", "1", "12", "0", "", "x", "1x", "-1", "4294967295", "4294967296", "All"};
  }
  static std::string random(Random &random) {
    return std::to_string(random.below(64));
  }
};

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

// A picture of 2 by 1 pixels, red and half see-through blue, as a PNG holds it.
std::string png() {
  constexpr std::array<std::uint8_t, 8> pixels{255, 0, 0, 255, 0, 0, 255, 128};
  std::string bytes;
  stbi_write_png_to_func(
      [](void *into, void *data, int size) {
        static_cast<std::string *>(into)->append(static_cast<const char *>(data),
                                                 static_cast<std::size_t>(size));
      },
      &bytes,
      2,
      1,
      4,
      pixels.data(),
      2 * 4);
  return bytes;
}

// What a cook uploaded and logged, its file port the node's own files.
std::string cooked(Running<Picture> &node, Names &names) {
  node.cook.uploads.clear();
  node.cook.said.clear();
  node.cooked();
  return std::format("uploads {}; logged {}",
                     show(node.cook.uploads, names),
                     show(node.cook.said, names));
}

} // namespace

void cases(Bench &bench) {
  bench.sweep(
      "Picture.cpp/mip_levels",
      [](const std::string &word) { return mip_levels(word); },
      domain::LevelsWord{});

  // What the node asks for, its file only where the param names one; a mip_levels the
  // part reads no count from refuses the bind.
  bench.check("Picture.cpp/Picture::bind", [](Rows &rows) {
    for (const Params &params : {Params{{"file", "/p/a.png"}, {"mip_levels", "all"}},
                                 Params{{"mip_levels", "1"}},
                                 Params{{"file", "/p/a.png"}, {"mip_levels", "none"}}}) {
      Running<Picture> node(params);
      std::string said;
      try {
        node.bound();
        said = show(node.bind.asked, rows.names());
      } catch (...) {
        said = thrown() + "; asked " + show(node.bind.asked, rows.names());
      }
      rows.row(std::format("{} -> {}", show(params, rows.names()), said));
    }
  });

  // A file decoded once per version: again only once its version moves; a file that does
  // not decode, or is not there, named in the log, nothing uploaded.
  bench.check("Picture.cpp/Picture::cook", [](Rows &rows) {
    const std::filesystem::path folder =
        planted(rows, {"a.png=" + png(), "b.png=no picture"});
    const auto step = [&](Running<Picture> &node, std::string_view what) {
      rows.row(here(std::format("{} -> {}", what, cooked(node, rows.names())), folder));
    };
    const auto made = [&](Params params) {
      auto node = std::make_unique<Running<Picture>>(std::move(params));
      node->bound();
      node->cook.file_port = &node->files;
      return node;
    };
    step(*made({{"mip_levels", "1"}}), "no file");
    const auto a = made({{"file", (folder / "a.png").string()}, {"mip_levels", "all"}});
    step(*a, "a.png, every level");
    step(*a, "a.png's version as before");
    a->files.versions = {1};
    step(*a, "a.png's version moved");
    step(*made({{"file", (folder / "b.png").string()}, {"mip_levels", "1"}}),
         "b.png, no picture");
    step(*made({{"file", (folder / "gone.png").string()}, {"mip_levels", "1"}}),
         "a file not there");
  });
}

} // namespace VP::bench
