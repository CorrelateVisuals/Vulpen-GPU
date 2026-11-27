#include "deck/Deck.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/deck: views it holds, one shown in its area: the view presented
// while it holds that one, else the one its shown param names, else the first hosted. A
// tab for each view it holds that is hosted, which presents it, and whose close mark lets
// it go; the shown param following the view presented; the focus following the pane it
// shows; showing none, it takes the focus at a press in its area.

namespace VP::bench {

namespace {

// The deck d in view v: its holds and shown params as given, none for unset; a pane of
// nodes a and b for p1, of c for p2, of e for p3; the keys' typed connected, or not; the
// view presented, none for empty.
struct Setup {
  std::optional<std::string> holds = "p1,p2";
  std::optional<std::string> shown;
  std::vector<std::string> hosted{"p1", "p2"};
  std::string presented;
  bool typed = true;
};

View own_view(const Setup &setup) {
  View view;
  view.name = "v";
  Node deck;
  deck.name = "d";
  if (setup.holds)
    deck.params.push_back({.key = "holds", .value = *setup.holds});
  if (setup.shown)
    deck.params.push_back({.key = "shown", .value = *setup.shown});
  view.nodes.push_back(std::move(deck));
  const auto connect = [&](std::string name, Endpoint from, std::vector<Endpoint> to) {
    view.connections.push_back(
        {.name = std::move(name), .from = std::move(from), .to = std::move(to)});
  };
  connect("tabs", {.node = "d", .port = "tabs"}, {{.node = "strip", .port = "items"}});
  connect("p1",
          {.node = "d", .port = "p1"},
          {{.node = "a", .port = "area"}, {.node = "b", .port = "area"}});
  connect("p2", {.node = "d", .port = "p2"}, {{.node = "c", .port = "area"}});
  connect("p3", {.node = "d", .port = "p3"}, {{.node = "e", .port = "area"}});
  if (setup.typed)
    connect("typed", {.node = "keys", .port = "typed"}, {{.node = "d", .port = "typed"}});
  if (!setup.presented.empty())
    connect("present", {.node = setup.presented}, {{.node = "screen", .port = "window"}});
  return view;
}

View top_view(const std::vector<std::string> &hosted) {
  View view;
  view.children.push_back({.name = "v"});
  for (const std::string &name : hosted)
    view.children.push_back({.name = name});
  return view;
}

// A tab as a strip draws it: its label, what a press sends, whether it is lit, its role
// and its close mark's line.
std::string tab_text(const VP_VIEW::Item &tab) {
  std::string text = tab.label;
  if (!tab.command.empty())
    text += " > " + tab.command;
  if (tab.current)
    text += ", current";
  if (tab.role)
    text += std::format(", role {}", *tab.role);
  if (!tab.close.empty())
    text += ", close " + tab.close;
  return text;
}

struct Framed {
  Running<Deck> node;
  FakeCommands commands;
  FakeInput input;

  explicit Framed(const Setup &setup) {
    node.bind.node_name = "d";
    node.bind.view_named = "v";
    if (setup.holds)
      node.bind.params.emplace_back("holds", *setup.holds);
    if (setup.shown)
      node.bind.params.emplace_back("shown", *setup.shown);
    node.bind.in_view = own_view(setup);
    node.bound();
    node.cook.views = {{"", top_view(setup.hosted)}};
    node.cook.command_port = &commands;
    node.cook.input_port = &input;
    node.port<VP_VIEW::Rect>("area") = {.offset = {0, 0}, .extent = {100, 100}};
  }
  // A frame of these events: what it sent, its tabs, and each pane's Rect.
  std::string cooked(Rows &rows, std::vector<Event> events = {}) {
    input.given = events;
    node.port<VP_VIEW::Typed>("typed").events = std::move(events);
    commands.sent.clear();
    node.cooked();
    std::string tabs;
    for (const VP_VIEW::Item &tab : node.port<VP_VIEW::Items>("tabs"))
      tabs += (tabs.empty() ? "" : "; ") + tab_text(tab);
    std::string rects;
    for (const std::string_view pane : {"p1", "p2", "p3"}) {
      const VP_VIEW::Rect &rect = node.port<VP_VIEW::Rect>(pane);
      rects += std::format(" {} {}x{}", pane, rect.extent.x, rect.extent.y);
    }
    return std::format(
        "sent {}; tabs {}; rects{}", show(commands.sent, rows.names()), tabs, rects);
  }
};

Event key(std::string name) {
  return {.kind = Event::Kind::key, .down = true, .name = std::move(name)};
}
Event pointer(glm::vec2 at) {
  return {.kind = Event::Kind::pointer, .at = at};
}
Event press() {
  return {.kind = Event::Kind::button, .down = true, .name = "left", .presses = 1};
}

} // namespace

void cases(Bench &bench) {
  // What it asks for: an output for each pane it connects, and the keys' typed where
  // they are connected.
  bench.check("Deck.cpp/Deck::bind", [](Rows &rows) {
    for (const bool typed : {true, false}) {
      Framed frame({.typed = typed});
      rows.row(std::format("typed {} -> {}",
                           typed ? "connected" : "not",
                           show(frame.node.bind.asked, rows.names())));
    }
  });

  // The view it shows: the one presented while it holds it, else its shown param's, else
  // the first it holds that is hosted; a tab for each held view hosted, the one presented
  // green; the shown param set to the view presented.
  bench.check("Deck.cpp/Deck::cook", [](Rows &rows) {
    const std::vector<std::pair<std::string_view, Setup>> setups{
        {"none presented, shown unset", {}},
        {"shown p2", {.shown = "p2"}},
        {"p2 presented", {.presented = "p2"}},
        {"p2 presented, shown p2", {.shown = "p2", .presented = "p2"}},
        {"p3 presented, which it does not hold", {.presented = "p3"}},
        {"shown p3, which it does not hold", {.shown = "p3"}},
        {"it holds p1 and p3, p3 not hosted", {.holds = "p1,p3"}},
        {"it holds p2 only", {.holds = "p2"}},
        {"it holds none", {.holds = std::nullopt}},
        {"p1 and p2 held, none hosted", {.hosted = {}}},
        {"holds with empty names", {.holds = ",p2,,p1,"}}};
    for (const auto &[what, setup] : setups) {
      Framed frame(setup);
      rows.row(std::format("{} -> {}", what, frame.cooked(rows)));
    }
  });

  // Its tabs and the view it shows written anew only as what it holds, the views hosted
  // or the one presented change: a row with none of them changed keeps what is there.
  bench.check("Deck.cpp/Deck::cook, frame after frame", [](Rows &rows) {
    Framed frame({});
    rows.row("p1 and p2 hosted -> " + frame.cooked(rows));
    frame.node.port<VP_VIEW::Items>("tabs").push_back({.label = "kept"});
    rows.row("the same again, the tabs written to since -> " + frame.cooked(rows));
    frame.node.cook.views[""] = top_view({"p2"});
    rows.row("p1 no longer hosted -> " + frame.cooked(rows));
    frame.node.cook.views[""] = top_view({"p2", "p1"});
    rows.row("p1 hosted again, after p2 -> " + frame.cooked(rows));
    frame.node.bind.in_view = own_view({.presented = "p2"});
    rows.row("p2 presented -> " + frame.cooked(rows));
    frame.node.bind.in_view = own_view({.presented = ""});
    rows.row("none presented -> " + frame.cooked(rows));
  });

  // The focus on a pane it hides goes to the node of the pane it shows listed where the
  // focused one was, and the focus on the deck to the pane's first node.
  bench.check("Deck.cpp/Deck::refocus", [](Rows &rows) {
    for (const std::string_view focus : {"a", "b", "d", "x"}) {
      Framed frame({});
      frame.cooked(rows);
      frame.node.port<VP_VIEW::Typed>("typed").focus = focus;
      frame.node.bind.in_view = own_view({.presented = "p2"});
      rows.row(std::format("focus on {}, p2 presented -> {}", focus, frame.cooked(rows)));
    }
    Framed untyped({.typed = false});
    untyped.cooked(rows);
    untyped.node.bind.in_view = own_view({.presented = "p2", .typed = false});
    rows.row("no keys connected, p2 presented -> " + untyped.cooked(rows));
  });

  // Showing none, a press in its area takes the focus, and escape lets it go; showing a
  // view, a press does not.
  bench.check("Deck.cpp/Deck::take_focus", [](Rows &rows) {
    Framed empty({.holds = std::nullopt});
    empty.node.port<VP_VIEW::Typed>("typed").focus = "x";
    rows.row("showing none, a press in its area -> " +
             empty.cooked(rows, {pointer({10, 10}), press()}));
    rows.row("a press past its area -> " +
             empty.cooked(rows, {pointer({150, 10}), press()}));
    empty.node.port<VP_VIEW::Typed>("typed").focus = "d";
    rows.row("in focus, a press in its area -> " +
             empty.cooked(rows, {pointer({10, 10}), press()}));
    rows.row("in focus, escape -> " + empty.cooked(rows, {key("escape")}));
    Framed showing({});
    showing.node.port<VP_VIEW::Typed>("typed").focus = "x";
    rows.row("showing p1, a press in its area -> " +
             showing.cooked(rows, {pointer({10, 10}), press()}));
  });
}

} // namespace VP::bench
