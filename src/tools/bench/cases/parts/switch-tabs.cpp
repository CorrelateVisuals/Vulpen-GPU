#include "switch-tabs/SwitchTabs.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/switch-tabs: the tabs of the two contents a switch holds, as one
// row: the shown half's as they are, the other's unlit, and a press on the other's
// current tab, or on one with no command of its own, sets the switch's shown param, the
// switch being the node that hands it its first. Written anew only as they change.

namespace VP::bench {

namespace {

// The view v: a switch sw handing the node tabs its first, its shown param as given,
// unset for none, after the contents a and b handing it their tabs. With connected false
// nothing hands tabs its first; with there false the view holds no node sw.
View view_of(std::optional<std::string> shown, bool connected = true, bool there = true) {
  View view;
  view.name = "v";
  for (const std::string_view half : {"first", "second"})
    view.connections.push_back(
        {.name = std::format("{}-tabs", half),
         .from = {.node = half == "first" ? "a" : "b", .port = "tabs"},
         .to = {{.node = "tabs", .port = std::format("{}-tabs", half)}}});
  if (there) {
    Node node;
    node.name = "sw";
    if (shown)
      node.params.push_back({.key = "shown", .value = *shown});
    view.nodes.push_back(std::move(node));
  }
  if (connected)
    view.connections.push_back({.name = "c",
                                .from = {.node = "sw", .port = "first"},
                                .to = {{.node = "tabs", .port = "first"}}});
  return view;
}

// Two tabs a half: its current one with no command, and another with one; the first
// half's lit in a role.
VP_VIEW::Items half(std::string_view label, std::string_view command) {
  return {{.label = std::format("{}1", label), .current = true, .role = 3},
          {.label = std::format("{}2", label), .command = std::string(command)}};
}

// A tab as the strip draws it: its label, what a press sends, and whether it is lit.
std::string tab_text(const VP_VIEW::Item &tab) {
  std::string text = tab.label;
  if (!tab.command.empty())
    text += " > " + tab.command;
  if (tab.current)
    text += ", current";
  if (tab.role)
    text += std::format(", role {}", *tab.role);
  return text;
}

struct Framed {
  Running<SwitchTabs> node;

  explicit Framed(View view) {
    node.bind.node_name = "tabs";
    node.bind.view_named = "v";
    node.bind.in_view = std::move(view);
    node.bound();
    node.port<VP_VIEW::Items>("first-tabs") = half("a", "open a2");
    node.port<VP_VIEW::Items>("second-tabs") = half("b", "open b2");
  }
  // The row its tabs make after a cook.
  std::string cooked() {
    node.cooked();
    std::string text;
    for (const VP_VIEW::Item &tab : node.port<VP_VIEW::Items>("tabs"))
      text += (text.empty() ? "" : "; ") + tab_text(tab);
    return text;
  }
};

} // namespace

void cases(Bench &bench) {
  bench.check("SwitchTabs.cpp/SwitchTabs::bind", [](Rows &rows) {
    Running<SwitchTabs> node;
    node.bound();
    rows.row(show(node.bind.asked, rows.names()));
  });

  // The switch's shown param, set or not; nothing handing it its first; a switch the
  // view holds no node of.
  bench.check("SwitchTabs.cpp/SwitchTabs::cook", [](Rows &rows) {
    const std::vector<std::pair<std::string, View>> views{
        {"shown first", view_of("first")},
        {"shown second", view_of("second")},
        {"shown unset", view_of(std::nullopt)},
        {"shown x", view_of("x")},
        {"no switch hands it its first", view_of("second", false)},
        {"its first from a node the view holds none of", view_of("second", true, false)}};
    for (const auto &[what, view] : views) {
      Framed frame(view);
      rows.row(std::format("{} -> {}", what, frame.cooked()));
    }
  });

  // Written anew only as they change: the tabs a half hands, or the switch's param; a
  // row the frame after a change shows it, and one with none keeps what is there.
  bench.check("SwitchTabs.cpp/SwitchTabs::cook, frame after frame", [](Rows &rows) {
    Framed frame(view_of("first"));
    rows.row("first shown -> " + frame.cooked());
    frame.node.port<VP_VIEW::Items>("tabs").push_back({.label = "kept"});
    rows.row("the same again, the tabs written to since -> " + frame.cooked());
    frame.node.port<VP_VIEW::Items>("second-tabs").push_back({.label = "b3"});
    rows.row("a tab more in the second half -> " + frame.cooked());
    frame.node.bind.in_view.nodes[0].params[0].value = "second";
    rows.row("the switch's param set to second -> " + frame.cooked());
  });
}

} // namespace VP::bench
