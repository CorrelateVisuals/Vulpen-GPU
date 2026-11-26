#include "image-file/ImageFile.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
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

// The library's parts/image-file: the picture file its file param names, shown in a tab
// of its own, whose close mark unsets the param. Opening a file, or none, switches its
// switch to the preview, or back, and clears the picture where none is left; the file
// decodes once a version, as the picture part's does.

namespace VP::bench {

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

// A node with the file port, commands and the view it runs in the case hands it.
struct Framed {
  Running<ImageFile> node;
  FakeCommands commands;

  explicit Framed(Params params) : node(std::move(params)) {
    node.cook.command_port = &commands;
  }
  void bound() {
    node.bound();
    node.cook.file_port = &node.files;
  }
};

} // namespace

void cases(Bench &bench) {
  // What the node asks for, and its tabs: one for a file, its close mark unsetting the
  // param; none without one.
  bench.check("ImageFile.cpp/ImageFile::bind", [](Rows &rows) {
    for (const Params &params :
         {Params{{"file", "/p/a.png"}, {"switch", "pane"}}, Params{{"switch", "pane"}}}) {
      Running<ImageFile> node(params);
      node.bound();
      rows.row(std::format("{} -> {}; tabs {}",
                           show(params, rows.names()),
                           show(node.bind.asked, rows.names()),
                           show(node.port<VP_VIEW::Items>("tabs"), rows.names())));
    }
  });

  // A file opened: the switch shows the preview, and the file decodes once a version; a
  // file not there named in the log; the file unset: the picture cleared, the switch
  // back.
  bench.check("ImageFile.cpp/ImageFile::cook", [](Rows &rows) {
    const std::filesystem::path folder = planted(rows, {"a.png=" + png()});
    const std::string picture = (folder / "a.png").string();
    const auto step = [&](Framed &frame, std::string_view what) {
      frame.commands.sent.clear();
      frame.node.cook.uploads.clear();
      frame.node.cook.said.clear();
      frame.node.cooked();
      rows.row(here(std::format("{} -> sent {}; uploads {}; logged {}",
                                what,
                                show(frame.commands.sent, rows.names()),
                                show(frame.node.cook.uploads, rows.names()),
                                show(frame.node.cook.said, rows.names())),
                    folder));
    };
    Framed frame({{"file", picture}, {"switch", "pane"}});
    frame.bound();
    step(frame, "a.png opened");
    step(frame, "its version as before");
    frame.node.files.versions = {1};
    step(frame, "its version moved");
    frame.node.bind.params = {{"switch", "pane"}};
    frame.bound();
    step(frame, "bound again with no file");
    step(frame, "and the frame after");
    Framed gone({{"file", (folder / "gone.png").string()}, {"switch", "pane"}});
    gone.bound();
    step(gone, "a file not there");
    Framed swapped({{"file", picture}, {"switch", "pane"}});
    swapped.bound();
    step(swapped, "a.png opened");
    swapped.node.bind.params = {{"file", (folder / "gone.png").string()},
                                {"switch", "pane"}};
    swapped.bound();
    step(swapped, "then a file not there opened in its place");
  });
}

} // namespace VP::bench
