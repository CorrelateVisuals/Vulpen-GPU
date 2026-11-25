#include "shared/Presented.h"

#include "tools/bench/Bench.h"
#include "tools/bench/Worlds.h"

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// The library's shared/Presented.h: which project a GUI beside the projects presents, and
// which deck shows which, so the modes, panes, deck and docking parts agree. The views
// are those an ide hosts: its own, two projects, one of which hosts a game, which hosts a
// level.

namespace VP::bench {

namespace {

using worlds::connection;
using worlds::node;

Child child(std::string name) {
  return {.name = name, .file = "/views/" + name + "/view.vlp"};
}

// The views by name, as the parts' view_of finds them, with the start view hosting the
// ide and the projects.
struct Hosted {
  std::map<std::string, View, std::less<>> views;

  Hosted() {
    views["top"] = {.name = "top",
                    .children = {child("ide"), child("wave"), child("game")}};
    views["ide"] = {.name = "ide"};
    views["wave"] = {.name = "wave"};
    views["game"] = {.name = "game", .children = {child("level")}};
    views["level"] = {.name = "level", .children = {child("room")}};
    views["room"] = {.name = "room"};
  }
  const View *operator()(std::string_view name) const {
    const auto found = views.find(name);
    return found == views.end() ? nullptr : &found->second;
  }
};

Node deck(std::string holds, std::string shown) {
  std::vector<Param> params;
  if (!holds.empty())
    params.push_back({std::string(holds_param), std::move(holds)});
  if (!shown.empty())
    params.push_back({std::string(shown_param), std::move(shown)});
  return node("deck", std::move(params));
}

} // namespace

void cases(Bench &bench) {
  bench.check("Presented.h/shown_by", [](Rows &rows) {
    const std::vector<std::pair<std::string, View>> windows{
        {"none", {}},
        {"a view presented",
         {.connections = {connection(std::string(presenting), "wave:", {"ide.window"})}}},
        {"a node's port named present",
         {.connections = {connection(std::string(presenting), "a.out", {"b.in"})}}},
        {"another connection",
         {.connections = {connection("window", "wave:", {"ide.window"})}}},
        {"two presenting, the first taken",
         {.connections = {connection(std::string(presenting), "wave:", {"a.in"}),
                          connection(std::string(presenting), "game:", {"b.in"})}}}};
    for (const auto &[what, view] : windows)
      rows.row(std::format("{} -> {}", what, escaped(shown_by(view))));
  });

  bench.check("Presented.h/holds", [](Rows &rows) {
    const Hosted hosted;
    for (const auto &[name, current] :
         std::vector<std::pair<std::string, std::string>>{{"game", "game"},
                                                          {"game", "level"},
                                                          {"game", "room"},
                                                          {"wave", "room"},
                                                          {"room", "game"},
                                                          {"gone", "gone"},
                                                          {"gone", "room"},
                                                          {"game", ""},
                                                          {"", ""}})
      rows.row(std::format("{} holds {} -> {}",
                           escaped(name),
                           escaped(current),
                           holds(hosted, name, current)));
  });

  bench.check("Presented.h/projects", [](Rows &rows) {
    const Hosted hosted;
    for (const std::string_view own : {"ide", "wave", "", "gone"}) {
      std::vector<std::string> names;
      for (const Child &each : projects(hosted.views.at("top"), own))
        names.push_back(each.name);
      rows.row(std::format("beside {} -> {}", escaped(own), show(names, rows.names())));
    }
  });

  bench.check("Presented.h/project_of", [](Rows &rows) {
    const Hosted hosted;
    for (const std::string_view current : {"wave", "game", "room", "ide", "gone", ""})
      rows.row(std::format(
          "current {} -> {}",
          escaped(current),
          escaped(project_of(hosted, projects(hosted.views.at("top"), "ide"), current))));
  });

  bench.check("Presented.h/wanted", [](Rows &rows) {
    const Hosted hosted;
    for (const auto &[current, presented] :
         std::vector<std::pair<std::string, std::string>>{{"room", ""},
                                                          {"room", "wave"},
                                                          {"ide", "wave"},
                                                          {"ide", ""},
                                                          {"", ""},
                                                          {"gone", "game"}})
      rows.row(std::format(
          "current {}, presented {} -> {}",
          escaped(current),
          escaped(presented),
          escaped(wanted(hosted, hosted.views.at("top"), "ide", current, presented))));
    const View alone{.name = "top", .children = {child("ide")}};
    rows.row(std::format("no project beside the ide -> {}",
                         escaped(wanted(hosted, alone, "ide", "ide", ""))));
  });

  bench.check("Presented.h/param_value", [](Rows &rows) {
    const Node held = node("deck", {{"holds", "a,b"}, {"shown", ""}, {"holds", "c"}});
    for (const std::string_view key : {"holds", "shown", "gone", ""})
      rows.row(std::format("{} -> {}", escaped(key), escaped(param_value(held, key))));
  });

  bench.sweep("Presented.h/split_names", split_names, domain::Word{});
  bench.check("Presented.h/split_names", [](Rows &rows) {
    for (const std::string_view joined : {"a,b", "a,,b", ",a,", "", ",", "a b,c", "a"})
      rows.row(std::format(
          "{} -> {}", escaped(joined), show(split_names(joined), rows.names())));
  });
  bench.sweep("Presented.h/joined_names", joined_names);
  bench.holds(
      "Presented.h/joined_names",
      "split, the names joined are the names, while none is empty or holds a comma",
      [](const std::vector<std::string> &names) {
        return std::ranges::any_of(names,
                                   [](const std::string &name) {
                                     return name.empty() ||
                                            name.find(',') != std::string::npos;
                                   }) ||
               split_names(joined_names(names)) == names;
      });

  bench.check("Presented.h/deck_shows", [](Rows &rows) {
    const auto hosts = [](std::string_view name) { return name != "closed"; };
    for (const auto &[held, shown, presented] :
         std::vector<std::tuple<std::string, std::string, std::string>>{
             {"a,b,c", "b", "c"},
             {"a,b,c", "b", "x"},
             {"a,b,c", "", ""},
             {"a,b,c", "x", ""},
             {"closed,b", "closed", ""},
             {"closed,b", "", "closed"},
             {"closed", "", ""},
             {"", "a", "a"},
             {"a,,b", "", ""}})
      rows.row(std::format("holds {}, shown {}, presented {} -> {}",
                           escaped(held),
                           escaped(shown),
                           escaped(presented),
                           escaped(deck_shows(deck(held, shown), presented, hosts))));
  });
}

} // namespace VP::bench
