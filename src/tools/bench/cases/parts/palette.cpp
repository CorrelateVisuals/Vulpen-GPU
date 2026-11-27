#include "palette/Palette.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Fakes.h"
#include "tools/bench/Worlds.h"

#include <cstddef>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/palette: a color for each role, from theme.ini, a row a role as
// role = red green blue, with alpha after when it is not opaque; read again once its
// text changes. A mistake stops the node, naming the line; every role needs a color.

namespace VP::bench {

namespace {

std::string color_text(const glm::vec4 &color) {
  return std::format("({} {} {} {})",
                     number_text(color.r),
                     number_text(color.g),
                     number_text(color.b),
                     number_text(color.a));
}

// The node, its theme.ini in a folder of its own.
struct Framed {
  Running<Palette> node;
  std::filesystem::path theme;

  Framed(const Rows &rows, std::string_view text) {
    const std::filesystem::path folder =
        planted(rows, {"theme.ini=" + std::string(text)});
    theme = folder / theme_file;
    node.bind.folder_path = folder.string();
    node.bound();
    node.files.node_files = {theme.string()};
    node.cook.file_port = &node.files;
  }
  // The colors it uploaded, a role each; or what it threw.
  std::string cooked(bool every = false) {
    try {
      node.cooked();
    } catch (...) {
      return thrown();
    }
    const auto *const colors =
        reinterpret_cast<const glm::vec4 *>(node.cook.written[0].data());
    std::string text = std::format("{} colors", node.cook.used[0]);
    for (std::size_t role = 0; role < (every ? node.cook.used[0] : 1); ++role)
      text += std::format(", {} {}", VP_VIEW::role_names[role], color_text(colors[role]));
    return text;
  }
};

// The library's own theme.
std::string library_theme(const Rows &rows) {
  return Files::text((rows.repo() / "src" / "runtime" / "recipes" / "vulpen" / "library" /
                      "parts" / "palette" / theme_file)
                         .string());
}

// The theme with its first line naming background swapped for another.
std::string with_background(const std::string &theme, std::string_view line) {
  const std::size_t at = theme.find("\nbackground");
  const std::size_t end = theme.find('\n', at + 1);
  return theme.substr(0, at + 1) + std::string(line) + theme.substr(end);
}

} // namespace

void cases(Bench &bench) {
  bench.check("Palette.cpp/Palette::bind", [](Rows &rows) {
    Running<Palette> node;
    node.bound();
    rows.row(show(node.bind.asked, rows.names()));
  });

  // The library's theme, every role's color; then the background row given otherwise.
  bench.check("Palette.cpp/colors_of", [](Rows &rows) {
    const std::string theme = library_theme(rows);
    rows.row("the library's theme -> " + Framed(rows, theme).cooked(true));
    const std::vector<std::pair<std::string_view, std::string>> themes{
        {"alpha after", with_background(theme, "background = 0 0 0 0.5")},
        {"-0 a channel", with_background(theme, "background = -0 0 0")},
        {"no =", with_background(theme, "background 0 0 0")},
        {"two channels", with_background(theme, "background = 0 0")},
        {"five channels", with_background(theme, "background = 0 0 0 1 1")},
        {"a channel past 1", with_background(theme, "background = 1.5 0 0")},
        {"a channel nan", with_background(theme, "background = nan 0 0")},
        {"words", with_background(theme, "background = x y z")},
        {"no color", with_background(theme, "background =")},
        {"a role that is none", with_background(theme, "sky = 0 0 1")},
        {"background twice", theme + "background = 0 0 0\n"},
        {"empty", ""}};
    for (const auto &[what, text] : themes)
      rows.row(std::format("{} -> {}", what, Framed(rows, text).cooked()));
  });

  // Read again only as its text changes: a theme edited, then broken, then as before the
  // break.
  bench.check("Palette.cpp/Palette::cook", [](Rows &rows) {
    const std::string theme = library_theme(rows);
    Framed frame(rows, theme);
    rows.row("the library's theme -> " + frame.cooked());
    const std::string blue = with_background(theme, "background = 0 0 1");
    Files::save(frame.theme.string(), blue);
    rows.row("its background edited to blue -> " + frame.cooked());
    Files::save(frame.theme.string(), with_background(theme, "background = 2 0 0"));
    rows.row("then broken -> " + frame.cooked());
    Files::save(frame.theme.string(), blue);
    rows.row("then blue again -> " + frame.cooked());
  });
}

} // namespace VP::bench
