#include "sound-record/SoundRecord.cpp"

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

// The library's parts/sound-record: what the speakers played, from the frame after the
// file was named, until it holds the seconds asked; then saved as a WAV file in the
// node's folder, said in the log, and no more recorded. A rebuild names the same file,
// which it saved already; another file starts again.

namespace VP::bench {

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

// A frame: what the speakers played the frame before, and what the node logged.
std::string cooked(Running<SoundRecord> &node, std::vector<float> played, Names &names) {
  FakeSound speakers;
  speakers.heard = std::move(played);
  node.cook.sound_port = &speakers;
  node.cook.said.clear();
  node.cooked();
  node.cook.sound_port = nullptr;
  return std::format("logged {}", show(node.cook.said, names));
}

// The samples a saved file holds, read back; or that it is not there.
std::string saved(const std::filesystem::path &file, Names &names) {
  if (!std::filesystem::exists(file))
    return "no file";
  return show(wave_read(Files::text(file), file.string()), names);
}

} // namespace

void cases(Bench &bench) {
  // What the node asks for; seconds above 0 and within what a WAV file holds, a nan
  // refused too.
  bench.check("SoundRecord.cpp/SoundRecord::bind", [](Rows &rows) {
    for (const std::string &seconds :
         std::vector<std::string>{"1",
                                  "0.0001",
                                  "0",
                                  "-1",
                                  "nan",
                                  "inf",
                                  std::to_string(longest_seconds),
                                  std::to_string(longest_seconds + 1)}) {
      Running<SoundRecord> node({{"file", "rec.wav"}, {"seconds", seconds}});
      std::string said;
      try {
        node.bound();
        said = show(node.bind.asked, rows.names());
      } catch (...) {
        said = thrown() + "; asked " + show(node.bind.asked, rows.names());
      }
      rows.row(std::format("seconds {} -> {}", seconds, said));
    }
  });

  // Five sample frames asked (0.0001 seconds): the frame the file was named records
  // nothing; then what played, up to the five; saved once full, and no more after; a
  // rebuild naming the same file records no more, another file starts again.
  bench.check("SoundRecord.cpp/SoundRecord::cook", [](Rows &rows) {
    const std::filesystem::path folder = planted(rows, {});
    const auto made = [&](std::string file) {
      auto node = std::make_unique<Running<SoundRecord>>(
          Params{{"file", std::move(file)}, {"seconds", "0.0001"}});
      node->bind.folder_path = folder.string();
      node->bound();
      node->cook.file_port = &node->files;
      return node;
    };
    const auto step = [&](Running<SoundRecord> &node,
                          std::vector<float> played,
                          std::string_view what) {
      rows.row(here(
          std::format("{} -> {}", what, cooked(node, std::move(played), rows.names())),
          folder));
    };
    const auto node = made("a.wav");
    step(*node, {0.5f, 0.5f}, "the frame a.wav was named");
    step(*node, {0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f}, "three frames played");
    step(
        *node, {0.4f, -0.4f, 0.5f, -0.5f, 0.6f, -0.6f}, "three more, two of them wanted");
    rows.row("a.wav -> " + saved(folder / "a.wav", rows.names()));
    step(*node, {0.7f, -0.7f}, "a frame after it saved");
    node->bound();
    step(*node, {0.7f, -0.7f}, "bound again, a.wav still named");
    node->bind.params = {{"file", "b.wav"}, {"seconds", "0.0001"}};
    node->bound();
    step(*node, {0.8f, -0.8f}, "bound with b.wav, its first frame");
    step(*node, std::vector<float>(10, 0.25f), "five frames played");
    rows.row("b.wav -> " + saved(folder / "b.wav", rows.names()));
    const auto none = std::make_unique<Running<SoundRecord>>(Params{{"seconds", "1"}});
    none->bound();
    rows.row("no file -> " + cooked(*none, {0.5f, 0.5f}, rows.names()));
  });
}

} // namespace VP::bench
