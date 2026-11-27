#include "shared/Picture.h"
#include "snapshot/Snapshot.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Worlds.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/snapshot: its picture saved as a PNG file wait frames after its
// file param names one, from the pixels its shader wrote the frame it shot, read back the
// frame after; with frames above 1, that many frames in a row, each file numbered.

namespace VP::bench {

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

// Two pixels as packUnorm4x8 packs them, red lowest: red, and blue half see-through.
constexpr std::array<std::uint32_t, 2> two_pixels{0xff0000ffu, 0x80ff0000u};

// A text with a folder as <there>, as here() shows the row's own.
std::string there(std::string text, const std::filesystem::path &folder) {
  constexpr std::string_view shown = "<there>";
  const std::string from = folder.string();
  for (std::size_t at = text.find(from); at != std::string::npos;
       at = text.find(from, at + shown.size()))
    text.replace(at, from.size(), shown);
  return text;
}

// The pictures in a folder, by name, each decoded: its size and its pixels' colors.
std::string pictures(const std::filesystem::path &folder) {
  std::vector<std::string> names;
  for (const auto &entry : std::filesystem::directory_iterator(folder))
    names.push_back(entry.path().filename().string());
  std::ranges::sort(names);
  std::string text;
  for (const std::string &name : names) {
    text += (text.empty() ? "" : "; ") + name;
    try {
      const Pixels picture = decoded(Files::text((folder / name).string()), name);
      text += std::format(" {} by {}:", picture.size.x, picture.size.y);
      for (const glm::u8vec4 &pixel : picture.rgba)
        text += std::format(" ({} {} {} {})", pixel.r, pixel.g, pixel.b, pixel.a);
    } catch (const std::exception &failure) {
      text += std::format(": {}", failure.what());
    }
  }
  return text.empty() ? "none" : text;
}

// The node in a folder of its own, its picture 2 by 1, what the GPU read back those two
// pixels.
struct Framed {
  Running<Snapshot> node;
  std::filesystem::path folder;

  Framed(const Rows &rows, Params params)
      : node(std::move(params)), folder(planted(rows, {})) {
    node.bind.folder_path = folder.string();
    node.bound();
    node.cook.file_port = &node.files;
    node.cook.pass.resize(sizeof(std::uint32_t));
    node.cook.size = {2, 1};
    std::vector<std::byte> bytes(sizeof two_pixels);
    std::memcpy(bytes.data(), two_pixels.data(), bytes.size());
    node.cook.readback = std::move(bytes);
  }
  // Bound again with these params, as a rebuild or a param set binds it.
  void bound(Params params) {
    node.bind.params = std::move(params);
    node.bound();
  }
  // A frame of this index: whether its shader shoots, what it logged, and the pictures
  // in its folder.
  std::string cooked(std::uint64_t index, Rows &rows) {
    node.cook.frame = index;
    node.cook.said.clear();
    node.cooked();
    std::uint32_t shoot = 0;
    std::memcpy(&shoot, node.cook.pass.data(), sizeof shoot);
    return here(std::format("frame {}: shoots {}; logged {}; files {}",
                            index,
                            shoot,
                            show(node.cook.said, rows.names()),
                            pictures(folder)),
                folder);
  }
};

Params lone(std::string_view file) {
  return {{"frames", "1"}, {"wait", "2"}, {"file", std::string(file)}};
}

} // namespace

void cases(Bench &bench) {
  // frames from 1 to 3600, else refused naming the param; a file, or none.
  bench.check("Snapshot.cpp/Snapshot::bind", [](Rows &rows) {
    for (const Params &params :
         {Params{{"frames", "1"}, {"wait", "30"}},
          Params{{"frames", "3600"}, {"wait", "30"}, {"file", "a.png"}},
          Params{{"frames", "0"}, {"wait", "30"}},
          Params{{"frames", "3601"}, {"wait", "30"}},
          Params{{"frames", "x"}, {"wait", "30"}}}) {
      Running<Snapshot> node(params);
      std::string said;
      try {
        node.bound();
        said = show(node.bind.asked, rows.names());
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("{} -> {}", show(params, rows.names()), said));
    }
  });

  // A picture saved wait frames after the file is named, read back the frame after it
  // shot; bound again naming the same file, as a rebuild binds it, nothing more; another
  // file, the same again.
  bench.check("Snapshot.cpp/Snapshot::cook", [](Rows &rows) {
    Framed frame(rows, lone("a.png"));
    for (std::uint64_t index = 10; index < 15; ++index)
      rows.row(frame.cooked(index, rows));
    frame.bound(lone("a.png"));
    rows.row("bound again, the same file: " + frame.cooked(15, rows));
    frame.bound(lone("b.png"));
    for (std::uint64_t index = 16; index < 20; ++index)
      rows.row("b.png named: " + frame.cooked(index, rows));
    frame.bound({{"frames", "1"}, {"wait", "2"}});
    rows.row("no file named: " + frame.cooked(20, rows));
  });

  // No file named, nothing shot, however many frames go by.
  bench.check("Snapshot.cpp/Snapshot::cook, no file named", [](Rows &rows) {
    Framed frame(rows, {{"frames", "1"}, {"wait", "1"}});
    for (std::uint64_t index = 0; index < 3; ++index)
      rows.row(frame.cooked(index, rows));
  });

  // With frames above 1, a picture each frame, numbered as wide as the count, the last's
  // line at info and the others' at debug; frames lowered below those taken while it
  // records, it stops at the next.
  bench.check("Snapshot.cpp/Snapshot::shoot, a recording", [](Rows &rows) {
    Framed frame(rows, {{"frames", "3"}, {"wait", "0"}, {"file", "c.png"}});
    for (std::uint64_t index = 0; index < 5; ++index)
      rows.row("3 frames: " + frame.cooked(index, rows));
    Framed lowered(rows, {{"frames", "12"}, {"wait", "0"}, {"file", "d.png"}});
    rows.row("12 frames: " + lowered.cooked(0, rows));
    rows.row("12 frames: " + lowered.cooked(1, rows));
    lowered.bound({{"frames", "1"}, {"wait", "0"}, {"file", "d.png"}});
    for (std::uint64_t index = 2; index < 5; ++index)
      rows.row("lowered to 1: " + lowered.cooked(index, rows));
  });

  // A file named by an absolute path is saved there, not in the node's folder.
  bench.check("Snapshot.cpp/Snapshot::shoot, an absolute path", [](Rows &rows) {
    const std::filesystem::path elsewhere = planted(rows, {});
    Framed frame(rows, lone((elsewhere / "e.png").string()));
    for (std::uint64_t index = 0; index < 4; ++index) {
      const std::string cooked = frame.cooked(index, rows); // saves before the listing
      rows.row(
          there(std::format("{}; there: {}", cooked, pictures(elsewhere)), elsewhere));
    }
  });

  // A picture larger than its pixels read back, as a window larger than the shader's
  // invocations, of no pixels, or read back before the GPU wrote any: named in the log,
  // and no file.
  bench.check("Snapshot.cpp/Snapshot::save, a picture it cannot save", [](Rows &rows) {
    for (const auto &[size, read] : std::vector<std::pair<glm::uvec2, std::size_t>>{
             {{3, 1}, 2}, {{0, 0}, 2}, {{2, 1}, 0}}) {
      Framed frame(rows, {{"frames", "1"}, {"wait", "0"}, {"file", "f.png"}});
      frame.node.cook.size = size;
      frame.node.cook.readback->resize(read * sizeof(std::uint32_t));
      std::string said = frame.cooked(0, rows);
      said += "; " + frame.cooked(1, rows);
      rows.row(std::format("{} by {}, {} read back -> {}", size.x, size.y, read, said));
    }
  });

  // A recording's file: its number before the ending, as wide as the count's.
  bench.check("Snapshot.cpp/numbered", [](Rows &rows) {
    for (const auto &[file, number, count] :
         std::vector<std::tuple<std::string, std::uint32_t, std::uint32_t>>{
             {"a.png", 1, 3},
             {"a.png", 7, 3600},
             {"shots/a.png", 12, 100},
             {"/abs/a.png", 3, 10},
             {"a", 2, 10},
             {".png", 1, 2}})
      rows.row(std::format(
          "{} {} of {} -> {}", file, number, count, numbered(file, number, count)));
  });
}

} // namespace VP::bench
