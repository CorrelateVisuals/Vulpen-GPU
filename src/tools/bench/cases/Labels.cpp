#include "shared/Labels.h"

#include "tools/bench/Bench.h"

#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>

// The library's shared/Labels.h: the Labels and Characters a part writes for glyphs in
// a frame, cut where its buffers end, and a name cut to the cells a box or a tab has.

namespace VP::bench {

namespace {

// What the buffers hold once written: each Label's first, count and role, with its
// characters as text and where each starts.
std::string written(const ::Labels &labels) {
  std::string text = std::format(
      "{} labels, {} characters:", labels.label_count, labels.character_count);
  for (std::uint32_t at = 0; at < labels.label_count; ++at) {
    const VP_VIEW::Label &label = labels.labels[at];
    std::string characters;
    for (std::uint32_t each = label.first; each < label.first + label.count; ++each) {
      const VP_VIEW::Character &character = labels.characters[each];
      characters.append(std::format("{}{}@{}",
                                    characters.empty() ? "" : " ",
                                    static_cast<char>(character.code),
                                    number_text(character.x)));
      if (character.label != at)
        characters.append(std::format("(label {})", character.label));
    }
    text.append(
        std::format(" [role {} first {}: {}]", label.role, label.first, characters));
  }
  return text;
}

} // namespace

void cases(Bench &bench) {
  // Buffers of three Labels and eight Characters, the font's cells 10 wide, filled in
  // the turns parts take.
  bench.check("Labels.h/Labels::add", [](Rows &rows) {
    std::array<VP_VIEW::Label, 3> labels{};
    std::array<VP_VIEW::Character, 8> characters{};
    ::Labels into{.labels = labels, .characters = characters, .cell_width = 10.0f};
    for (const std::string_view text : {"ab", "", "cdefgh", "ij"}) {
      const bool added = into.add({.role = 7}, text);
      rows.row(std::format("add {} -> {}; {}", escaped(text), added, written(into)));
    }
    ::Labels none{.cell_width = 10.0f};
    rows.row(std::format("add to no buffers -> {}", none.add({}, "a")));
  });
  bench.check("Labels.h/Labels::append", [](Rows &rows) {
    std::array<VP_VIEW::Label, 3> labels{};
    std::array<VP_VIEW::Character, 8> characters{};
    ::Labels into{.labels = labels, .characters = characters, .cell_width = 10.0f};
    into.add({.role = 1}, "ab");
    for (const std::string_view text : {"c", "", "de"}) {
      into.append(text);
      rows.row(std::format("append {} -> {}", escaped(text), written(into)));
    }
    into.add({.role = 2}, "fg");
    into.append("hij");
    rows.row(std::format("add fg, append hij -> {}", written(into)));
  });

  bench.sweep(
      "Labels.h/fit",
      [](std::string_view name, std::size_t cells) {
        std::string into = "kept";
        fit(into, name, cells);
        return into;
      },
      domain::NodePath{});
  bench.check("Labels.h/fit", [](Rows &rows) {
    for (const std::string_view name : {"projects/triangles/triangle-params/triangle",
                                        "graph - Knowledgebase-CLI",
                                        "projects/a/",
                                        "a/b",
                                        "-a",
                                        "a-",
                                        "ab",
                                        "a/ b",
                                        "a/  b",
                                        "a- "})
      for (const std::size_t cells : {0, 3, 4, 8, 12, 20, 30, 50}) {
        std::string into;
        fit(into, name, cells);
        rows.row(std::format("{} in {} -> {}", escaped(name), cells, escaped(into)));
      }
  });
  bench.holds(
      "Labels.h/fit",
      "a name that fits stays whole; else ... and an end of it that starts at a / or a -",
      [](std::string_view name, std::size_t cells) {
        std::string into;
        fit(into, name, cells);
        if (name.size() <= cells || into == name)
          return into == name;
        const std::string_view end = std::string_view(into).substr(3);
        return into.starts_with("...") && name.ends_with(end) && end.size() < name.size();
      },
      domain::NodePath{});
}

} // namespace VP::bench
