#pragma once

#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Only Docking.cpp includes this header, so what it holds stays in the unnamed namespace
// of the one file that builds the part, as the class does: what the docking part keeps
// of the panels as a person left them, between runs, in a file beside the view's
// manifest, as the graph keeps where its boxes are: the edits that moved the panels,
// then the params of the nodes that seat them.
namespace {

constexpr std::string_view kept_folder = ".vulpen"; // which no build or scan reads
constexpr std::string_view kept_file = "layout.log";
constexpr std::string_view kept_head =
    "# The panels as a person left them, which panel restore replays as the view\n"
    "# starts as its manifest has it: the edits that moved them, then their nodes'\n"
    "# params. Deleting the file shows the manifest's own.\n";
// The recipes of the nodes the panels are made of, whose params the file keeps: the
// docks seating them, the panels, the stacks holding several contents, the editors.
constexpr std::array<std::string_view, 4> laid_recipes{"dock", "panel", "stack",
                                                       "text-area"};

// Whether a line the docking part sends makes the panels other than by a param: a node
// added, moved or removed, or a connection made or let go. The params the file keeps
// whole, at its end, and closing a file edits no view.
bool structural(std::string_view line) {
  return line.starts_with("node ") || line.starts_with("connect ") ||
         line.starts_with("disconnect ");
}

// The file's text: the edits, then the params of the panels' nodes as the view holds them
// now, a line each, as param set sets them, as a dock's seam and a panel's strip.
std::string kept_text(std::span<const std::string> edits, const VP::View &view) {
  std::string text(kept_head);
  for (const std::string &edit : edits)
    text.append(edit).append(1, '\n');
  for (const VP::Node &node : view.nodes)
    if (std::ranges::find(laid_recipes, node.recipe) != laid_recipes.end())
      for (const VP::Param &param : node.params)
        if (const std::size_t dot = param.key.rfind('.'); dot != std::string::npos)
          text += std::format("param set {}.{} {} {}\n",
                              node.name,
                              std::string_view(param.key).substr(0, dot),
                              std::string_view(param.key).substr(dot + 1),
                              param.value);
  return text;
}

// The lines a kept file holds, past its comments and its blank lines.
std::vector<std::string> kept_lines(std::string_view text) {
  std::vector<std::string> lines;
  while (!text.empty()) {
    const std::size_t end = std::min(text.find('\n'), text.size());
    const std::string_view line = text.substr(0, end);
    if (!line.empty() && !line.starts_with('#'))
      lines.emplace_back(line);
    text.remove_prefix(std::min(end + 1, text.size()));
  }
  return lines;
}

// Whether the view runs as its manifest holds it, its nodes, their params and its
// connections the same, so the kept panels go onto the view they were moved from, not
// onto one a replay of the log, as a restart's, arranged already.
bool as_written(const VP::View &running, const VP::View &written) {
  const auto same_param = [](const VP::Param &a, const VP::Param &b) {
    return a.key == b.key && a.value == b.value;
  };
  const auto same_end = [](const VP::Endpoint &a, const VP::Endpoint &b) {
    return a.node == b.node && a.port == b.port;
  };
  return std::ranges::equal(running.nodes,
                            written.nodes,
                            [&](const VP::Node &a, const VP::Node &b) {
                              return a.name == b.name && a.recipe == b.recipe &&
                                     std::ranges::equal(a.params, b.params, same_param);
                            }) &&
         std::ranges::equal(running.connections,
                            written.connections,
                            [&](const VP::Connection &a, const VP::Connection &b) {
                              return a.name == b.name && same_end(a.from, b.from) &&
                                     std::ranges::equal(a.to, b.to, same_end);
                            });
}

} // namespace
