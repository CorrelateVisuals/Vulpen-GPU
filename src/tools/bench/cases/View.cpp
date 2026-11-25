#include "runtime/View.h"

#include "runtime/Edits.h"
#include "tools/bench/Bench.h"
#include "tools/bench/Worlds.h"

#include <string>
#include <string_view>
#include <vector>

// runtime/View.h: the graph's rules, written once (C02). Pure, so each is swept on its
// own, with the claims RV08 and the save make: a node's name and its folder lead back to
// each other, and the words a save writes read back as the node.

namespace VP::bench {

namespace domain {
// A node's files as its folder holds them: the stages a draw or a dispatch takes, C++,
// and others.
struct StageFiles {};
} // namespace domain

template <> struct Gen<domain::StageFiles> {
  using type = std::vector<std::string>;
  static std::vector<type> edges() {
    return {{"A.vert", "A.frag"},
            {},
            {"A.comp"},
            {"A.vert"},
            {"A.frag"},
            {"A.vert", "A.geom", "A.frag"},
            {"A.vert", "A.tesc", "A.frag"},
            {"A.vert", "A.tese", "A.frag"},
            {"A.vert", "A.tesc", "A.tese", "A.frag"},
            {"A.geom"},
            {"A.comp", "A.vert"},
            {"A.cpp", "A.h"},
            {"A.vert.bak", "A.frag~"},
            {"A.VERT"}};
  }
  static type random(Random &random) {
    constexpr std::array<std::string_view, 8> files{
        "A.comp", "A.vert", "A.frag", "A.geom", "A.tesc", "A.tese", "A.cpp", "B.vert"};
    type found;
    for (std::uint64_t count = random.below(4); count > 0; --count)
      found.emplace_back(files[random.below(files.size())]);
    return found;
  }
};

void cases(Bench &bench) {
  bench.sweep("View.h/hidden", hidden);
  bench.sweep("View.h/is_name", is_name, domain::Name{});
  bench.sweep("View.h/is_path", is_path, domain::NodePath{});
  bench.holds(
      "View.h/is_path",
      "a path is names joined by dots",
      [](std::string_view text) {
        for (std::string_view rest = text;;) {
          const std::size_t dot = rest.find('.');
          if (!is_name(rest.substr(0, dot)))
            return !is_path(text);
          if (dot == std::string_view::npos)
            break;
          rest.remove_prefix(dot + 1);
        }
        return is_path(text);
      },
      domain::NodePath{});

  bench.sweep("View.h/path_of", path_of, domain::NodePath{});
  bench.holds(
      "View.h/path_of",
      "a node's folder and its name lead back to each other (RV08)",
      [](std::string_view node) {
        if (!is_path(node))
          return true;
        std::string back = path_of(node).generic_string();
        std::ranges::replace(back, '/', '.');
        return back == node;
      },
      domain::NodePath{});

  bench.sweep("View.h/within", within, domain::NodePath{}, domain::NodePath{});
  bench.holds(
      "View.h/within",
      "inside it, or it",
      [](std::string_view node, std::string_view other) {
        const bool by_dots = node == other || node.starts_with(std::string(other) + ".");
        return within(node, other) == by_dots;
      },
      domain::NodePath{},
      domain::NodePath{});

  bench.sweep("View.h/port_node", port_node, domain::NodePath{});
  bench.sweep("View.h/missing_stages", missing_stages, domain::StageFiles{});
  bench.sweep(
      "View.h/holds_stage",
      [](const std::vector<std::string> &files, std::string_view stage) {
        return holds_stage(Node{.name = "n", .files = files}, stage);
      },
      domain::StageFiles{},
      domain::Word{});

  // What a save writes of a node reads back as the node, through the edits a load runs.
  bench.holds("View.h/words_of",
              "the words a save writes read back as the node",
              [](const View &view) {
                for (const Node &node : view.nodes) {
                  Node read{.name = node.name};
                  for (const Word &word : words_of(node))
                    Edits::word(read, word.key, word.value);
                  Names one;
                  Names other;
                  if (show(read, one) !=
                      show(Node{.name = node.name,
                                .recipe = node.recipe,
                                .operator_name = node.operator_name,
                                .invocations = node.invocations,
                                .invocations_port = node.invocations_port,
                                .vertex_count = node.vertex_count,
                                .instance_count = node.instance_count,
                                .params = node.params,
                                .images = node.images,
                                .log = node.log},
                           other))
                    return false;
                }
                return true;
              });
  bench.sweep("View.h/words_of", [](const View &view) {
    std::vector<std::string> said;
    for (const Node &node : view.nodes)
      for (const Word &word : words_of(node))
        said.push_back(std::format("{}: {} = {}", node.name, word.key, word.value));
    return said;
  });

  bench.sweep("View.h/draws_into_window",
              [](const View &view) { return draws_into_window(view); });
  bench.sweep(
      "View.h/is_top",
      [](const View &view, std::string_view node) { return is_top(view, node); },
      domain::Type{},
      domain::NodePath{});
  bench.sweep(
      "View.h/folder_in",
      [](const View &view, std::string_view node) { return folder_in(view, node); },
      domain::Type{},
      domain::NodePath{});
  bench.sweep(
      "View.h/Endpoint::text",
      [](std::string_view node, std::string_view port) {
        return Endpoint{std::string(node), std::string(port)}.text();
      },
      domain::NodePath{},
      domain::Name{});
}

} // namespace VP::bench
