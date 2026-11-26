#include "sound-file/SoundFile.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Worlds.h"

#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/sound-file: the WAV file its file param names, played into the
// speakers at its volume, a frame's samples a frame, from the start again where it loops;
// read again from its start once the file's version moves. A file it cannot read is named
// in the log, and it plays silence.

namespace VP::bench {

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

// What a cook added to the speakers, taking that many sample frames, and logged.
std::string cooked(Running<SoundFile> &node, std::size_t frames, Names &names) {
  FakeSound speakers;
  speakers.taken = frames;
  node.cook.sound_port = &speakers;
  node.cook.said.clear();
  node.cooked();
  node.cook.sound_port = nullptr;
  return std::format(
      "added {}; logged {}", show(speakers.added, names), show(node.cook.said, names));
}

} // namespace

void cases(Bench &bench) {
  // What the node asks for, its file only where the param names one; loop yes or no only.
  bench.check("SoundFile.cpp/SoundFile::bind", [](Rows &rows) {
    for (const Params &params :
         {Params{{"file", "/s/a.wav"}, {"loop", "yes"}, {"volume", "1"}},
          Params{{"loop", "no"}, {"volume", "0.5"}},
          Params{{"file", "/s/a.wav"}, {"loop", "maybe"}, {"volume", "1"}}}) {
      Running<SoundFile> node(params);
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

  // Two sample frames in the file, three a frame: played once with silence after, or
  // looped; at half volume; from the start again once the version moves; silence for a
  // file not there, named in the log; nothing asked of the speakers without a file.
  bench.check("SoundFile.cpp/SoundFile::cook", [](Rows &rows) {
    const std::filesystem::path folder = planted(
        rows, {"a.wav=" + wave_write(std::vector<float>{0.5f, -0.5f, 0.25f, -0.25f})});
    const std::string wave = (folder / "a.wav").string();
    const auto made = [&](Params params) {
      auto node = std::make_unique<Running<SoundFile>>(std::move(params));
      node->bound();
      node->cook.file_port = &node->files;
      return node;
    };
    const auto step = [&](Running<SoundFile> &node,
                          std::size_t frames,
                          std::string_view what) {
      rows.row(here(
          std::format(
              "{}, {} frames -> {}", what, frames, cooked(node, frames, rows.names())),
          folder));
    };
    const auto once = made({{"file", wave}, {"loop", "no"}, {"volume", "1"}});
    step(*once, 3, "a.wav once");
    step(*once, 3, "a.wav once, played out");
    once->files.versions = {1};
    step(*once, 1, "a.wav once, its version moved");
    step(*made({{"file", wave}, {"loop", "yes"}, {"volume", "1"}}), 3, "a.wav looped");
    step(*made({{"file", wave}, {"loop", "no"}, {"volume", "0.5"}}),
         2,
         "a.wav at half volume");
    step(
        *made(
            {{"file", (folder / "gone.wav").string()}, {"loop", "yes"}, {"volume", "1"}}),
        2,
        "a file not there");
    const auto none = made({{"loop", "no"}, {"volume", "1"}});
    none->cook.said.clear();
    none->cooked();
    rows.row(std::format("no file -> logged {}", show(none->cook.said, rows.names())));
  });
}

} // namespace VP::bench
