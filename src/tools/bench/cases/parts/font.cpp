#include "font/Font.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Worlds.h"

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/font: its face, a file of its folder, baked at its height param
// times the zoom .vulpen/font.ini keeps into an atlas of a cell a glyph, as wide as the
// widest, with each glyph's advance; baked again only as the face, the height or the zoom
// change, or the atlas is emptied. zoom in, zoom out and zoom reset write the .ini, which
// a mistake in refuses naming its line.

namespace VP::bench {

namespace {

// Each check's rows are its record here: Font.cpp, which this file includes, names
// the atlas's rows of glyphs rows, and MSVC refuses a name that hides another.
using Params = std::vector<std::pair<std::string, std::string>>;

constexpr std::string_view face = "RobotoMono-Regular.ttf";

// The library's face, as its part's folder holds it.
std::string face_bytes(const Rows &record) {
  return Files::text((record.repo() / "src" / "runtime" / "recipes" / "vulpen" /
                      "library" / "parts" / "font" / face)
                         .string());
}

// The node in a folder holding the face and, where given, a .vulpen/font.ini.
struct Framed {
  Running<Font> node;
  std::filesystem::path place;

  Framed(const Rows &record, Params params, std::string_view kept = {})
      : node(std::move(params)) {
    std::vector<std::string> tree{std::string(face) + "=" + face_bytes(record)};
    if (!kept.empty())
      tree.push_back(".vulpen/font.ini=" + std::string(kept));
    place = planted(record, tree);
    bound();
    node.cook.file_port = &node.files;
  }
  // Bound, as a schedule build binds it, its files where its folder holds them.
  void bound() {
    node.bound();
    node.files.node_files.clear();
    for (const std::string &file : node.bind.files)
      node.files.node_files.push_back((place / file).string());
  }
  // A frame: the font it wrote, its atlas when it baked one, and what it logged.
  std::string cooked(Rows &record) {
    node.cook.uploads.clear();
    node.cook.said.clear();
    try {
      node.cooked();
    } catch (...) {
      return thrown();
    }
    const auto &font =
        *reinterpret_cast<const VP_VIEW::Font *>(node.cook.written.back().data());
    const VP_VIEW::Metrics &metrics = node.port<VP_VIEW::Metrics>("metrics");
    return std::format("cell {}x{} base {}x{} atlas {}x{} columns {} first {} count {}; "
                       "advance of A {}, of i {}; uploads {}; logged {}",
                       font.cell.x,
                       font.cell.y,
                       font.base.x,
                       font.base.y,
                       font.atlas.x,
                       font.atlas.y,
                       font.columns,
                       font.first,
                       font.count,
                       metrics.advance['A' - font.first],
                       metrics.advance['i' - font.first],
                       show(node.cook.uploads, record.names()),
                       show(node.cook.said, record.names()));
  }
  // A zoom command by its index as bind registered it: 0 in, 1 out, 2 reset; then the
  // .ini it wrote.
  std::string zoomed(std::uint32_t command) {
    FakeCall call;
    call.running = {command};
    call.file_port = &node.files;
    try {
      node.called(call);
    } catch (...) {
      return thrown();
    }
    return escaped(Files::text((place / ".vulpen" / "font.ini").string()));
  }
};

Params dropped() {
  return {{"face", std::string(face)}, {"height", "18"}};
}

} // namespace

void cases(Bench &bench) {
  // Its ports first, then its face: a file of its folder named without one; none for an
  // empty face, which the loader names.
  bench.check("Font.cpp/Font::bind", [](Rows &record) {
    for (const Params &params : {dropped(),
                                 Params{{"face", "fonts/Mono.ttf"}, {"height", "18"}},
                                 Params{{"face", "C:\\Mono.ttf"}, {"height", "18"}},
                                 Params{{"face", ""}, {"height", "18"}}}) {
      Running<Font> node(params);
      std::string said;
      try {
        node.bound();
        said = show(node.bind.asked, record.names());
      } catch (...) {
        said = thrown();
      }
      record.row(std::format("{} -> {}", show(params, record.names()), said));
    }
  });

  // The zoom a row gives, from 0.5 to 4; none, 1; each mistake refused naming its row.
  bench.check("Font.cpp/zoom_of", [](Rows &record) {
    for (const std::string_view text : {"",
                                        "zoom = 1.5",
                                        "# a comment\nzoom = 2\n",
                                        "zoom = 1.5\nzoom = 3",
                                        "zoom = 0.5",
                                        "zoom = 4",
                                        "zoom = 4.5",
                                        "zoom = 0.25",
                                        "zoom = nan",
                                        "zoom = 1 2",
                                        "size = 2",
                                        "zoom"}) {
      std::string said;
      try {
        said = number_text(zoom_of(text));
      } catch (...) {
        said = thrown();
      }
      record.row(std::format("{} -> {}", escaped(text), said));
    }
  });

  // Baked once, and not again a frame later, nor at a bind of the same face; again at a
  // bind once the face's bytes differ, another face's or its own saved over, which a save
  // under the view's folder brings as its build ends; again as the zoom the .ini keeps
  // changes the height, and as the atlas is emptied, as image clear empties it; an .ini
  // that holds a mistake named in the log, the font at its height.
  bench.check("Font.cpp/Font::cook", [](Rows &record) {
    Framed frame(record, dropped());
    record.row("the first frame -> " + frame.cooked(record));
    record.row("the frame after -> " + frame.cooked(record));
    frame.bound();
    record.row("bound again, the same face -> " + frame.cooked(record));
    Files::save((frame.place / "Other.ttf").string(), face_bytes(record) + "\n");
    frame.node.bind.params = {{"face", "Other.ttf"}, {"height", "18"}};
    frame.bound();
    record.row("bound again, a face of other bytes -> " + frame.cooked(record));
    Files::save((frame.place / "Other.ttf").string(), face_bytes(record));
    record.row("its file saved over, not bound since -> " + frame.cooked(record));
    frame.bound();
    record.row("then bound again -> " + frame.cooked(record));
    Files::save((frame.place / ".vulpen" / "font.ini").string(), "zoom = 2\n");
    record.row("the .ini saved, zoom 2 -> " + frame.cooked(record));
    frame.node.cook.filled.clear();
    record.row("the atlas emptied -> " + frame.cooked(record));
    Files::save((frame.place / ".vulpen" / "font.ini").string(), "zoom = 9\n");
    record.row("the .ini saved, zoom 9 -> " + frame.cooked(record));
  });

  // A face it cannot bake, named: one not there, or empty, a height that makes a cell of
  // no size, and a file that is no font, or a font cut short.
  bench.check("Font.cpp/Font::bake", [](Rows &record) {
    Framed gone(record, {{"face", "Gone.ttf"}, {"height", "18"}});
    record.row("a face not there -> " + gone.cooked(record));
    Framed empty(record, {{"face", "Empty.ttf"}, {"height", "18"}});
    Files::save((empty.place / "Empty.ttf").string(), "");
    record.row("an empty face -> " + empty.cooked(record));
    Framed flat(record, {{"face", std::string(face)}, {"height", "0"}});
    record.row("height 0 -> " + flat.cooked(record));
    // A collection whose one font lies past its 16 bytes.
    std::string collection = "ttcf";
    collection += std::string{
        '\0', '\1', '\0', '\0', '\0', '\0', '\0', '\1', '\0', '\0', '\3', '\xe8'};
    Framed ttc(record, {{"face", "Many.ttc"}, {"height", "18"}});
    Files::save((ttc.place / "Many.ttc").string(), collection);
    record.row("a collection whose font lies past its end -> " + ttc.cooked(record));
    // What stb_truetype would read past: a file of no font, too few bytes for one, and
    // the face cut short, its tables past its end.
    const std::string bytes = face_bytes(record);
    for (const auto &[what, made] : std::vector<std::pair<std::string_view, std::string>>{
             {"a file of no font", "this file holds text, not a font"},
             {"three bytes", "abc"},
             {"the face's first 16 bytes", bytes.substr(0, 16)},
             {"the face's first 1000 bytes", bytes.substr(0, 1000)}}) {
      Framed broken(record, {{"face", "Broken.ttf"}, {"height", "18"}});
      Files::save((broken.place / "Broken.ttf").string(), made);
      record.row(std::format("{} -> {}", what, broken.cooked(record)));
    }
  });

  // zoom in, out and reset write the .ini whole, from what it holds now, a step of 1.1
  // at most 4 and at least 0.5; an .ini that holds a mistake refuses them, naming its
  // row.
  bench.check("Font.cpp/Font::command", [](Rows &record) {
    Framed frame(record, dropped());
    record.row("zoom in -> " + frame.zoomed(0));
    record.row("zoom in again -> " + frame.zoomed(0));
    record.row("zoom out -> " + frame.zoomed(1));
    record.row("zoom reset -> " + frame.zoomed(2));
    Framed most(record, dropped(), "zoom = 3.9\n");
    record.row("zoom in at 3.9 -> " + most.zoomed(0));
    Framed least(record, dropped(), "zoom = 0.52\n");
    record.row("zoom out at 0.52 -> " + least.zoomed(1));
    Framed broken(record, dropped(), "zoom = x\n");
    record.row("zoom in, the .ini broken -> " + broken.zoomed(0));
  });
}

} // namespace VP::bench
