#include "tools/bench/Worlds.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// tools/bench/Worlds.h: the views and trees of files the cases run the engine on,
// and how a row shows them. Two views shown alike that differ would hide a change in
// every record of a view; a folder left unnamed in a row would move it with every run.

namespace VP::bench {

namespace {

// The sandbox's loop of links, which bench.py plants in its root: above a thread's own
// folder when two run at once.
std::filesystem::path loop_of(const Rows &rows) {
  const std::filesystem::path loop = rows.root() / "fixtures" / "loop";
  return std::filesystem::exists(loop) ? loop
                                       : rows.root().parent_path() / "fixtures" / "loop";
}

} // namespace

void cases(Bench &bench) {
  bench.check("Worlds.h/here", [](Rows &rows) {
    const auto shows = [&](std::string_view text, std::string_view folder) {
      rows.row(std::format("({}, {}) -> {}",
                           escaped(text),
                           escaped(folder),
                           escaped(here(std::string(text), folder))));
    };
    shows("in /a/b and /a/b/c", "/a/b");
    shows("/a/a/a", "/a/a");
    shows("/a/bc", "/a/b");
    shows("", "/a");
    shows("/a/b", "");
  });

  bench.check("Worlds.h/tree_of", [](Rows &rows) {
    rows.row("a planted tree -> " + tree_of(planted(rows, {"a=1", "d/", "d/b=2", "e/"})));
    rows.row("the sandbox's loop of links -> " + tree_of(loop_of(rows)));
    rows.row("a folder that is not there -> " + tree_of(rows.root() / "none"));
    std::vector<std::string> many;
    for (std::size_t index = 0; index < shown_entries + 5; ++index)
      many.push_back(std::format("f{:02}={}", index, index));
    rows.row(std::format("{} files -> ", many.size()) + tree_of(planted(rows, many)));
  });

  bench.holds("Worlds.h/Shown<View>",
              "two views show alike exactly when they are alike whole",
              [](const View &one, const View &other) {
                Names names;
                return (show(one, names) == show(other, names)) ==
                       (worlds::whole(one) == worlds::whole(other));
              });

  bench.check("Worlds.h/changes", [](Rows &rows) {
    const std::vector<View> views = worlds::small();
    const View &chain = views[2];
    View after = chain;
    after.nodes.push_back(worlds::node("d"));
    after.nodes[0].params = {{"x", "2"}};
    after.connections.pop_back();
    std::swap(after.nodes[1], after.nodes[2]);
    rows.row("chain, then d added, a changed, bc gone, b and c swapped -> " +
             worlds::changes(chain, after));
    rows.row("chain and itself -> " + worlds::changes(chain, chain));
    rows.row("empty, then chain -> " + worlds::changes(views[1], chain));
  });

  bench.check("Worlds.h/chain_of", [](Rows &rows) {
    const View view = worlds::chain_of(1000);
    rows.row(std::format("1000 -> {} nodes, {} connections, {} inside another",
                         view.nodes.size(),
                         view.connections.size(),
                         std::ranges::count_if(view.nodes, [](const Node &node) {
                           return node.name.find('.') != std::string::npos;
                         })));
    rows.row("0 -> " + worlds::whole(worlds::chain_of(0)));
    rows.row("2 -> " + worlds::whole(worlds::chain_of(2)));
  });

  // As the claims over the real thing take them; a claim, not a count, so a project added
  // moves no row.
  bench.check("Worlds.h/manifests", [](Rows &rows) {
    const std::vector<std::filesystem::path> found = worlds::manifests(rows.repo());
    const bool as_said =
        !found.empty() && std::ranges::is_sorted(found) &&
        std::ranges::all_of(found, [](const std::filesystem::path &path) {
          const std::string text = path.generic_string();
          return path.filename() == manifest_file &&
                 (text.starts_with("projects/") ||
                  text.starts_with("src/runtime/recipes/")) &&
                 text.find("/.") == std::string::npos;
        });
    rows.row(
        std::format("every one a view.vlp of projects/ or the recipes, none in a hidden "
                    "folder, sorted -> {}",
                    as_said));
  });
}

} // namespace VP::bench
