#pragma once

#include "baseclasses/Platform.h"
#include "runtime/Text.h"
#include "runtime/View.h"
#include "tools/bench/Bench.h"
#include "tools/bench/Domains.h"
#include "tools/bench/Shown.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// The bench's worlds: views as the engine holds them, shown whole in a row, so two
// views show alike exactly when they are alike; small ones made by hand, which a sweep
// takes as a View's edges; random ones; every view.vlp of the tree, for a claim over the
// real thing; a view of a thousand nodes, for scale; and trees of files in a row's own
// folder of the sandbox, a tree's "path=text" a file and "path/" a folder.

namespace VP::bench {

template <> struct Shown<Endpoint> {
  static std::string text(const Endpoint &end, Names &) {
    return end.text();
  }
};

template <> struct Shown<Node> {
  static std::string text(const Node &node, Names &names) {
    std::string text = node.name;
    for (const Word &word : words_of(node))
      text += std::format(" {}={}", word.key, escaped(word.value));
    if (!node.files.empty())
      text += " files=" + show(node.files, names);
    if (!node.where.empty())
      text += " at " + escaped(node.where);
    return text;
  }
};

template <> struct Shown<Connection> {
  static std::string text(const Connection &connection, Names &names) {
    return std::format("{}: {} -> {}",
                       connection.name,
                       connection.from.text(),
                       show(connection.to, names));
  }
};

template <> struct Shown<Child> {
  static std::string text(const Child &child, Names &names) {
    return std::format("{} {}", child.name, show(child.file, names));
  }
};

namespace worlds {
// Every part of the view, so a view changed in any way reads otherwise.
inline std::string whole(const View &view) {
  Names names;
  return std::format("{{view {} nodes {} connections {} children {}}}",
                     escaped(view.name),
                     show(view.nodes, names),
                     show(view.connections, names),
                     show(view.children, names));
}
} // namespace worlds

// By its name and a hash of the whole, so a row stays short and two views show alike
// exactly when they are alike; Worlds.h holds the hand-made ones whole.
template <> struct Shown<View> {
  static std::string text(const View &view, Names &) {
    return std::format(
        "view {} #{:08x}", escaped(view.name), hash_of(worlds::whole(view)) >> 32);
  }
};

namespace worlds {

// What an edit changed, as a row shows it: each node, connection and hosted view added,
// changed or gone, and the nodes' order where only it changed; unchanged for none.
inline std::string changes(const View &before, const View &after) {
  Names names;
  std::vector<std::string> said;
  const auto compare =
      [&](const auto &olds, const auto &news, std::string_view kind, auto name) {
        for (const auto &item : news) {
          const auto old = std::ranges::find(olds, name(item), name);
          if (old == olds.end())
            said.push_back(std::format("+{} {}", kind, show(item, names)));
          else if (show(*old, names) != show(item, names))
            said.push_back(std::format("~{} {}", kind, show(item, names)));
        }
        for (const auto &item : olds)
          if (std::ranges::find(news, name(item), name) == news.end())
            said.push_back(std::format("-{} {}", kind, name(item)));
      };
  compare(before.nodes, after.nodes, "node", [](const Node &node) { return node.name; });
  compare(before.connections,
          after.connections,
          "connection",
          [](const Connection &connection) { return connection.name; });
  compare(before.children, after.children, "view", [](const Child &child) {
    return child.name;
  });
  std::vector<std::string> was;
  std::vector<std::string> is;
  for (const Node &node : before.nodes)
    if (std::ranges::find(after.nodes, node.name, &Node::name) != after.nodes.end())
      was.push_back(node.name);
  for (const Node &node : after.nodes)
    if (std::ranges::find(before.nodes, node.name, &Node::name) != before.nodes.end())
      is.push_back(node.name);
  if (was != is)
    said.push_back("order " + show(is, names));
  std::string text;
  for (const std::string &each : said)
    text.append(text.empty() ? "" : "; ").append(each);
  return text.empty() ? "unchanged" : text;
}

inline Node node(std::string name, std::vector<Param> params = {}) {
  return {.name = std::move(name), .params = std::move(params)};
}

inline Connection
connection(std::string name, std::string from, std::vector<std::string> to) {
  Connection made{.name = std::move(name)};
  const auto end = [](const std::string &text) {
    const std::size_t dot = text.rfind('.');
    return text.ends_with(':') ? Endpoint{text.substr(0, text.size() - 1), {}}
                               : Endpoint{text.substr(0, dot), text.substr(dot + 1)};
  };
  made.from = end(from);
  for (const std::string &each : to)
    made.to.push_back(end(each));
  return made;
}

// Views a person makes in a first hour: none, one node, a chain, nodes inside a node, a
// node that uses a recipe of the library as it is, a hosted view whose window a node
// takes.
inline std::vector<View> small() {
  View empty{.name = "empty"};
  View one{.name = "one", .nodes = {node("a", {{"x", "1"}})}};
  View chain{.name = "chain",
             .nodes = {node("a"), node("b"), node("c")},
             .connections = {connection("ab", "a.out", {"b.in"}),
                             connection("bc", "b.out", {"c.in"})}};
  View nested{.name = "nested",
              .nodes = {node("ui"),
                        node("ui.panel", {{"rows", "4"}}),
                        node("ui.panel.text"),
                        node("b")},
              .connections = {connection("rects", "ui.panel.rects", {"b.rects"})}};
  View uses{.name = "uses", .nodes = {node("probe"), node("tool")}};
  uses.nodes.back().recipe = "lamp";
  uses.nodes.back().params = {{"light.tint", "1,1,1"}};
  View hosting{.name = "hosting",
               .children = {{.name = "game", .file = "game/view.vlp"}},
               .nodes = {node("show")},
               .connections = {connection("window", "game:", {"show.picture"})}};
  return {one, empty, chain, nested, uses, hosting};
}

// A view of n nodes in a chain, each reading the one before, every tenth inside the one
// before it.
inline View chain_of(std::size_t count) {
  View view{.name = "many"};
  std::string previous;
  for (std::size_t index = 0; index < count; ++index) {
    std::string name = index % 10 == 9 ? previous + ".n" + std::to_string(index)
                                       : "n" + std::to_string(index);
    if (!previous.empty())
      view.connections.push_back(
          connection("c" + std::to_string(index), previous + ".out", {name + ".in"}));
    view.nodes.push_back(node(name, {{"i", std::to_string(index)}}));
    previous = std::move(name);
  }
  return view;
}

// Every manifest of the tree: each project's, and each recipe's in the library and the
// other groups, sorted, by its path from the tree's top. A hidden folder is passed over
// whole, as what vulpen keeps there, the trash and a session's files, is no project.
inline std::vector<std::filesystem::path> manifests(const std::filesystem::path &repo) {
  std::vector<std::filesystem::path> found;
  for (const char *const top : {"projects", "src/runtime/recipes"})
    for (auto entry = std::filesystem::recursive_directory_iterator(repo / top);
         entry != std::filesystem::recursive_directory_iterator();
         ++entry)
      if (entry->path().filename().string().starts_with('.'))
        entry.disable_recursion_pending();
      else if (entry->is_regular_file() && entry->path().filename() == manifest_file)
        found.push_back(entry->path().lexically_relative(repo));
  std::ranges::sort(found);
  return found;
}

} // namespace worlds

template <> struct Gen<View> {
  using type = View;
  static std::vector<View> edges() {
    return worlds::small();
  }
  // A few nodes, some inside others, and connections between their ports.
  static View random(Random &random) {
    View view{.name = "drawn"};
    const std::size_t count = random.below(5);
    for (std::size_t index = 0; index < count; ++index) {
      std::string name(1, static_cast<char>('a' + index));
      if (index > 0 && random.below(3) == 0)
        name = view.nodes[random.below(index)].name + "." + name;
      view.nodes.push_back(worlds::node(std::move(name)));
    }
    for (std::size_t index = 1; index < count; ++index)
      if (random.below(2) == 0)
        view.connections.push_back(worlds::connection(std::format("c{}", index),
                                                      view.nodes[index - 1].name + ".out",
                                                      {view.nodes[index].name + ".in"}));
    return view;
  }
};

// Trees of files, which a row plants in a folder of its own.
namespace domain {
struct Tree {};
struct TreePath {};
} // namespace domain

template <> struct Gen<domain::Tree> {
  using type = std::vector<std::string>;
  static std::vector<type> edges() {
    return {{"a=1"},
            {},
            {"a=1", "a.tmp=mine"},
            {"d/", "d/b=2"},
            {"d/", "d/b=2", "d.tmp/", "d.tmp/keep=mine"},
            {"a=1", "b=2"},
            {"d/"},
            {"d/e/f=3"},
            {"a=1", "x.tmp/", "x.tmp/keep=mine"}};
  }
  static type random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

template <> struct Gen<domain::TreePath> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"a",
            "b",
            "d",
            "d/b",
            "d/e",
            "a.tmp",
            "d.tmp",
            "x",
            "d/x",
            "",
            "..",
            "a/b",
            "d/../a",
            "d/e/f"};
  }
  static std::string random(Random &random) {
    return edges()[random.below(edges().size())];
  }
};

// A folder no row of this process used yet, holding the tree.
inline std::filesystem::path planted(const Rows &rows,
                                     const std::vector<std::string> &tree) {
  std::filesystem::path folder;
  for (std::size_t index = 0;; ++index) {
    folder = rows.root() / std::format("t{}", index);
    if (!std::filesystem::exists(folder))
      break;
  }
  Files::make(folder);
  for (const std::string &entry : tree) {
    const std::size_t equals = entry.find('=');
    if (equals == std::string::npos) {
      if (!std::filesystem::exists(folder / entry))
        Files::make(folder / entry);
    } else {
      Files::save(folder / entry.substr(0, equals), entry.substr(equals + 1));
    }
  }
  return folder;
}

// A text with the row's folder as <here>, as any path the call names shows. An empty
// folder names nothing: found at every place, it would be put in for ever.
inline std::string here(std::string text, const std::filesystem::path &folder) {
  constexpr std::string_view shown = "<here>";
  const std::string from = folder.string();
  for (std::size_t at = from.empty() ? std::string::npos : text.find(from);
       at != std::string::npos;
       at = text.find(from, at + shown.size()))
    text.replace(at, from.size(), shown);
  return text;
}

// A project copied as a checkout holds it: each file keeps its time, so the debug build's
// mirror is no older than its files, and a bind takes its code as built from them, as
// in a run (saved_since_built), not as saved since and waiting for a live build.
inline void copy_built(const std::filesystem::path &from,
                       const std::filesystem::path &to) {
  Files::copy(from, to);
  std::error_code unread;
  for (auto entry = std::filesystem::recursive_directory_iterator(from, unread);
       entry != std::filesystem::recursive_directory_iterator();
       entry.increment(unread))
    if (entry->is_regular_file(unread))
      std::filesystem::last_write_time(to / entry->path().lexically_relative(from),
                                       entry->last_write_time(unread),
                                       unread);
}

// The tree under the folder, each file with its text, each folder with a /, each link
// with what it leads to, sorted; past this many, cut, as a link loop can make many. A
// folder gone, or a file where it was, says so: a call that removed the folder would
// otherwise show as one that emptied it.
inline constexpr std::size_t shown_entries = 40;
inline std::string tree_of(const std::filesystem::path &folder) {
  std::vector<std::string> entries;
  std::error_code failed;
  if (!std::filesystem::is_directory(folder, failed))
    return std::filesystem::exists(folder, failed) ? "not a folder" : "no folder";
  for (auto entry = std::filesystem::recursive_directory_iterator(folder, failed);
       !failed && entry != std::filesystem::recursive_directory_iterator();
       entry.increment(failed)) {
    const std::string path = entry->path().lexically_relative(folder).generic_string();
    if (entry->is_symlink())
      entries.push_back(path + " -> " +
                        std::filesystem::read_symlink(entry->path()).generic_string());
    else if (entry->is_directory())
      entries.push_back(path + "/");
    else
      entries.push_back(path + "=" + Files::text(entry->path()));
  }
  std::ranges::sort(entries);
  if (const std::size_t count = entries.size(); count > shown_entries) {
    entries.resize(shown_entries);
    entries.push_back(std::format("... ({} in all)", count));
  }
  return std::format("[{}]", joined(entries));
}

// What a call said and the tree after it.
template <class F> std::string after(const std::filesystem::path &folder, F &&call) {
  std::string said = "done";
  try {
    call();
  } catch (const std::exception &failure) {
    said = "throws " + escaped(here(failure.what(), folder));
  }
  return said + "; then " + tree_of(folder);
}

} // namespace VP::bench
