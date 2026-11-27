#include "panes/Panes.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"

#include <format>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/panes: a pane, a node of the recipe its pane param names, for each
// view hosted beside its GUI while a deck holds it, its rooms from that deck and its keys
// from the keys part's typed; each view's window to its pane while its deck shows it, and
// the presented one's to the ports its to param names too; the decks letting go of views
// closed and taking views hosted; each pane's picture handed the pointer as its interact
// param says.

namespace VP::bench {

struct PanesWords {
  using type = std::string_view (*)(std::string_view);
  friend constexpr type reach(PanesWords);
};
template struct Reach<PanesWords, &::Panes::words>;

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

// The ide's, but interact, which a row sets.
Params ide(std::string interact = "no") {
  return {{"to", "ide.screen.picture"},
          {"pane", "still-image"},
          {"rooms", "image.area,still.area"},
          {"keys", "image.typed,still.typed"},
          {"pictures", "image.picture,still.picture"},
          {"interact", std::move(interact)}};
}

// A pane ide.<view>-pane, its room from the deck's port of the view; with no deck, from
// a node that is no deck.
struct Pane {
  std::string view;
  std::string deck = "ide.deck";
  std::string node; // its name, if not ide.<view>-pane
};

// The ide's view as the lines before left it: its decks and what they hold, its panes,
// the keys' typed reaching the panes part and each pane, the view presented to the screen
// and to its pane's pictures where present_pane says so, and the windows to panes.
struct Setup {
  std::vector<std::string> hosted{"p1", "p2"};
  std::string current;
  std::vector<std::pair<std::string, std::string>> decks{{"ide.deck", "p1,p2"}};
  std::vector<Pane> panes;
  std::string presented;
  bool present_pane = true;
  std::vector<std::string> windows;
  bool typed = true;
  bool interacting = false;        // each pane's picture's interact param set
  std::vector<std::string> others; // nodes of other recipes
};

std::string pane_of(const Pane &pane) {
  return pane.node.empty() ? std::format("ide.{}-pane", pane.view) : pane.node;
}

std::vector<Endpoint> inside(const std::string &pane, std::string_view port) {
  return {{pane + ".image", std::string(port)}, {pane + ".still", std::string(port)}};
}

View own_view(const Setup &setup) {
  View view;
  view.name = "ide";
  for (const auto &[deck, holds] : setup.decks) {
    Node node{.name = deck, .recipe = "deck"};
    if (!holds.empty())
      node.params.push_back({.key = "holds", .value = holds});
    view.nodes.push_back(std::move(node));
  }
  for (const std::string &other : setup.others)
    view.nodes.push_back({.name = other, .recipe = "rects"});
  Connection typed{
      .name = "typed", .from = {"ide.keys", "typed"}, .to = {{"ide.panes", "typed"}}};
  for (const Pane &pane : setup.panes) {
    const std::string node = pane_of(pane);
    Node made{.name = node, .recipe = "still-image"};
    if (setup.interacting)
      made.params.push_back({.key = "image.interact", .value = "yes"});
    view.nodes.push_back(std::move(made));
    view.connections.push_back({.name = std::format("{}-room", short_name(node)),
                                .from = pane.deck.empty()
                                            ? Endpoint{"ide.dock", "a"}
                                            : Endpoint{pane.deck, pane.view},
                                .to = inside(node, "area")});
    for (Endpoint &end : inside(node, "typed"))
      typed.to.push_back(std::move(end));
  }
  if (setup.typed)
    view.connections.push_back(std::move(typed));
  const auto pane_named = [&](std::string_view shown) {
    const auto found = std::ranges::find(setup.panes, shown, &Pane::view);
    return found == setup.panes.end() ? std::string() : pane_of(*found);
  };
  if (!setup.presented.empty()) {
    Connection present{.name = "present",
                       .from = {setup.presented, ""},
                       .to = {{"ide.screen", "picture"}}};
    if (const std::string pane = pane_named(setup.presented);
        setup.present_pane && !pane.empty())
      for (Endpoint &end : inside(pane, "picture"))
        present.to.push_back(std::move(end));
    view.connections.push_back(std::move(present));
  }
  for (const std::string &shown : setup.windows) {
    const std::string pane = pane_named(shown);
    view.connections.push_back({.name = std::format("window-{}", short_name(pane)),
                                .from = {shown, ""},
                                .to = inside(pane, "picture")});
  }
  return view;
}

View top_view(const std::vector<std::string> &hosted) {
  View view;
  view.children.push_back({.name = "ide"});
  for (const std::string &name : hosted)
    view.children.push_back({.name = name});
  return view;
}

std::vector<std::string> words_of(std::string_view line) {
  std::vector<std::string> words;
  for (const auto word : std::views::split(line, ' '))
    if (!std::ranges::empty(word))
      words.emplace_back(word.begin(), word.end());
  return words;
}

// A port as a line names it, node.port, or a view's window as view:.
Endpoint endpoint_of(std::string_view text) {
  if (text.ends_with(':'))
    return {std::string(text.substr(0, text.size() - 1)), ""};
  const std::size_t dot = text.rfind('.');
  return {std::string(text.substr(0, dot)), std::string(text.substr(dot + 1))};
}

// The node a param line names, or the node holding it, as a pane holds its picture,
// whose params it keeps as dotted keys; and the key as that node holds it.
std::pair<Node *, std::string> param_of(View &own, std::string node, std::string key) {
  for (;;) {
    const auto found = std::ranges::find(own.nodes, node, &Node::name);
    if (found != own.nodes.end())
      return {&*found, key};
    const std::size_t dot = node.rfind('.');
    if (dot == std::string::npos)
      throw std::logic_error("no node holds " + node);
    key = node.substr(dot + 1) + "." + key;
    node.resize(dot);
  }
}

// A line panes sends, applied as the engine applies it while it runs, so the next step
// reads the view it left: a param set or unset, a node added, or removed with each end
// inside it, a connection made or dropped, the current view switched. A node moved keeps
// its place: the order of nodes means nothing to panes.
void apply(View &own, std::optional<std::string> &current, std::string_view line) {
  std::vector<std::string> word = words_of(line);
  if (word.size() == 3 && word[0] == "view" && word[1] == "switch") {
    current = word[2];
    return;
  }
  if (word.size() < 3 || word[0] != "ide:")
    throw std::logic_error(std::format("no line to the ide: {}", line));
  word.erase(word.begin());
  const auto gone = [](const Endpoint &end, std::string_view node) {
    return end.node == node || VP::within(end.node, node);
  };
  if (word[0] == "param" && word[1] == "set") {
    auto [node, key] = param_of(own, word[2], word[3]);
    std::erase_if(node->params, [&](const Param &param) { return param.key == key; });
    node->params.push_back({.key = key, .value = word.at(4)});
  } else if (word[0] == "param" && word[1] == "unset") {
    auto [node, key] = param_of(own, word[2], word[3]);
    std::erase_if(node->params, [&](const Param &param) { return param.key == key; });
  } else if (word[0] == "node" && word[1] == "add") {
    Node node{.name = word[2],
              .recipe = word.at(3).substr(std::string_view("recipe=").size())};
    for (std::size_t at = 4; at < word.size(); ++at) {
      const std::string set = word[at].substr(std::string_view("param=").size());
      node.params.push_back(
          {.key = set.substr(0, set.find('=')), .value = set.substr(set.find('=') + 1)});
    }
    own.nodes.push_back(std::move(node));
  } else if (word[0] == "node" && word[1] == "remove") {
    const std::string &name = word.at(3);
    std::erase_if(own.nodes, [&](const Node &node) {
      return node.name == name || VP::within(node.name, name);
    });
    for (Connection &connection : own.connections)
      std::erase_if(connection.to, [&](const Endpoint &end) { return gone(end, name); });
    std::erase_if(own.connections, [&](const Connection &connection) {
      return connection.to.empty() || gone(connection.from, name);
    });
  } else if (word[0] == "connect") {
    Connection made{.name = word[1], .from = endpoint_of(word.at(2))};
    for (std::size_t at = 3; at < word.size(); ++at)
      made.to.push_back(endpoint_of(word[at]));
    own.connections.push_back(std::move(made));
  } else if (word[0] == "disconnect") {
    std::erase_if(own.connections, [&](const Connection &connection) {
      return connection.name == word[1];
    });
  } else if (!(word[0] == "node" && word[1] == "move")) {
    throw std::logic_error(std::format("the case applies no line {}", line));
  }
}

struct Framed {
  Running<Panes> node;
  FakeCommands commands;
  // The view as the frame began, kept while lines edit it, as Views keeps a view until
  // the frame's end applies its edits; the edited copies in between go as the next line
  // replaces them.
  std::optional<decltype(node.cook.views)::node_type> began;

  explicit Framed(Params params = ide(), bool typed = true) : node(std::move(params)) {
    node.bind.node_name = "ide.panes";
    node.bind.view_named = "ide";
    node.bind.in_view = own_view({.typed = typed});
    node.bound();
    node.cook.command_port = &commands;
    commands.answers = [this](std::string_view line) {
      View next = node.cook.views.at("ide");
      apply(next, commands.now, line);
      auto was = node.cook.views.extract("ide");
      node.cook.views.emplace("ide", std::move(next));
      if (!began)
        began = std::move(was);
      return std::string();
    };
  }
  void set(const Setup &setup) {
    node.cook.views.clear();
    node.cook.views.emplace("", top_view(setup.hosted));
    node.cook.views.emplace("ide", own_view(setup));
    commands.now = setup.current;
  }
  // Another's line, as a deck's close mark sends it.
  void edit(std::string_view line) {
    apply(node.cook.views.at("ide"), commands.now, line);
  }
  void host(const std::vector<std::string> &hosted) {
    node.cook.views.at("") = top_view(hosted);
  }
  // Frames until one sends nothing, at most five: what each sent.
  std::string frames(Rows &rows) {
    std::string text;
    for (std::size_t count = 1; count <= 5; ++count) {
      commands.sent.clear();
      began.reset();
      node.cooked();
      text += std::format(
          "{}{} {}", text.empty() ? "" : "; ", count, show(commands.sent, rows.names()));
      if (commands.sent.empty())
        break;
    }
    return text;
  }
  std::string frames(Rows &rows, const Setup &setup) {
    set(setup);
    return frames(rows);
  }
};

std::string button_text(const VP_VIEW::Items &buttons) {
  std::string text;
  for (const VP_VIEW::Item &button : buttons)
    text += std::format("{}{} > {}{}",
                        text.empty() ? "" : "; ",
                        button.label,
                        button.command,
                        button.current ? ", current" : "");
  return text;
}

} // namespace

void cases(Bench &bench) {
  bench.sweep("Panes.cpp/short_name", short_name, domain::Type{});
  bench.sweep("Panes.cpp/split_port", split_port, domain::Type{});
  bench.sweep("Panes.cpp/Panes::words", reach(PanesWords{}), domain::Type{});

  // What it asks for, its button, and its params refused: a pane, rooms and pictures it
  // needs, interact yes or no.
  bench.check("Panes.cpp/Panes::bind", [](Rows &rows) {
    const auto bound = [&](std::string_view what, Params params, bool typed = true) {
      std::string said;
      try {
        Framed frame(std::move(params), typed);
        said = std::format("asked {}; buttons {}",
                           show(frame.node.bind.asked, rows.names()),
                           button_text(frame.node.port<VP_VIEW::Items>("buttons")));
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("{} -> {}", what, said));
    };
    bound("the ide's, interact no", ide());
    bound("the ide's, interact yes", ide("yes"));
    bound("no keys connected", ide(), false);
    Params unset = ide();
    std::erase_if(unset, [](const auto &param) {
      return param.first == "interact" || param.first == "to" || param.first == "keys";
    });
    bound("interact, to and keys unset", unset);
    bound("interact maybe", ide("maybe"));
    for (const std::string_view key : {"pane", "rooms", "pictures"}) {
      Params missing = ide();
      for (auto &param : missing)
        if (param.first == key)
          param.second = key == "pane" ? "" : ",,";
      bound(std::format("{} empty", key), missing);
    }
  });

  // A run's first frames, the views as each line left them: the deck takes the views
  // hosted, a pane is added for each, joining the keys' typed, the view presented, the
  // newest, reaches the screen and its pane; then nothing more is sent.
  bench.check("Panes.cpp/Panes::cook, a run's first frames", [](Rows &rows) {
    Framed frame;
    rows.row("p1 and p2 hosted, the deck holding none -> " +
             frame.frames(rows, {.decks = {{"ide.deck", ""}}}));
    Framed two_decks;
    rows.row("two decks holding none -> " +
             two_decks.frames(rows, {.decks = {{"ide.deck", ""}, {"ide.other", ""}}}));
    Framed none;
    rows.row("none hosted -> " +
             none.frames(rows, {.hosted = {}, .decks = {{"ide.deck", ""}}}));
  });

  // From the settled run of p1 and p2, p2 presented: each change of the views hosted, the
  // current view or its view sends the lines that follow it, frame after frame, until
  // nothing more is sent.
  bench.check("Panes.cpp/Panes::cook, as views come and go", [](Rows &rows) {
    const auto from_settled = [&](std::string_view what, const auto &change) {
      Framed frame;
      frame.frames(rows, {.decks = {{"ide.deck", ""}}});
      change(frame);
      rows.row(std::format("{} -> {}", what, frame.frames(rows)));
    };
    from_settled("nothing changed", [](Framed &) {});
    from_settled("p1 closed", [](Framed &frame) { frame.host({"p2"}); });
    from_settled("p2, presented, closed", [](Framed &frame) { frame.host({"p1"}); });
    from_settled("both closed", [](Framed &frame) { frame.host({}); });
    from_settled("p3 hosted", [](Framed &frame) { frame.host({"p1", "p2", "p3"}); });
    from_settled("p3 hosted and switched to, as view load does", [](Framed &frame) {
      frame.host({"p1", "p2", "p3"});
      frame.commands.now = "p3";
    });
    from_settled("lines go to p1", [](Framed &frame) { frame.commands.now = "p1"; });
    from_settled("lines go to the ide, its own",
                 [](Framed &frame) { frame.commands.now = "ide"; });
    from_settled("the deck lets go of p1",
                 [](Framed &frame) { frame.edit("ide: param set ide.deck holds p2"); });
    from_settled("the deck lets go of p2, presented",
                 [](Framed &frame) { frame.edit("ide: param set ide.deck holds p1"); });
    from_settled("the deck lets go of both",
                 [](Framed &frame) { frame.edit("ide: param unset ide.deck holds"); });
    from_settled("p2 moved to a new deck, as docking moves it", [](Framed &frame) {
      frame.edit("ide: node add ide.other recipe=deck param=holds=p2 param=shown=p2");
      frame.edit("ide: param set ide.deck holds p1");
      frame.edit("ide: disconnect p2-pane-room");
      frame.edit("ide: connect p2-pane-room ide.other.p2 ide.p2-pane.image.area "
                 "ide.p2-pane.still.area");
    });
    from_settled("p2's pane removed by hand",
                 [](Framed &frame) { frame.edit("ide: node remove keep ide.p2-pane"); });
    from_settled("the present connection dropped by hand",
                 [](Framed &frame) { frame.edit("ide: disconnect present"); });
  });

  // An edit of its own view alone, which leaves the view where it was: the panes follow
  // it all the same.
  bench.check("Panes.cpp/Panes::cook, its view edited in place", [](Rows &rows) {
    Framed frame;
    frame.frames(rows, {.decks = {{"ide.deck", ""}}});
    const View *const was = &frame.node.cook.views.at("ide");
    frame.edit("ide: param set ide.deck holds p2");
    rows.row(
        std::format("the deck lets go of p1, the view {} -> {}",
                    was == &frame.node.cook.views.at("ide") ? "where it was" : "moved",
                    frame.frames(rows)));
  });

  // At the frame after the first, a view a deck holds that no line hosts is let go of, as
  // a deck kept as a run starts anew; only once.
  bench.check("Panes.cpp/Panes::cook, the frame after the first", [](Rows &rows) {
    Framed frame;
    frame.set({.hosted = {"p1"},
               .decks = {{"ide.deck", "p1,gone"}, {"ide.empty", "lost"}},
               .panes = {{.view = "p1"}},
               .presented = "p1"});
    rows.row("1 the deck holds p1 and gone, another lost, p1 hosted -> " +
             frame.frames(rows));
    rows.row("2 -> " + frame.frames(rows));
    frame.edit("ide: param set ide.empty holds lost");
    rows.row("3 the other holding lost again -> " + frame.frames(rows));
  });

  // The view presented keeps a tab: let go of, the first view a deck holds that is hosted
  // is presented; with none held, it joins a deck again.
  bench.check("Panes.cpp/Panes::present_next", [](Rows &rows) {
    const Setup two{.decks = {{"ide.deck", "p1"}, {"ide.other", "p2"}},
                    .panes = {{.view = "p1"}, {.view = "p2", .deck = "ide.other"}},
                    .presented = "p2",
                    .windows = {"p1"}};
    const auto run = [&](std::string_view what, std::string_view line) {
      Framed frame;
      frame.frames(rows, two);
      frame.edit(line);
      rows.row(std::format("{} -> {}", what, frame.frames(rows)));
    };
    run("two decks settled, nothing changed", "ide: node move ide.deck ide.other");
    run("the other deck lets go of p2, presented", "ide: param unset ide.other holds");
    run("the first deck lets go of p1", "ide: param unset ide.deck holds");
    Framed none;
    none.frames(rows, two);
    none.edit("ide: param unset ide.deck holds");
    none.edit("ide: param unset ide.other holds");
    rows.row("both decks let go -> " + none.frames(rows));
    Framed no_decks;
    no_decks.frames(rows, two);
    no_decks.edit("ide: node remove keep ide.deck");
    no_decks.edit("ide: node remove keep ide.other");
    rows.row("both decks removed -> " + no_decks.frames(rows));
  });

  // The panes there are: each node of the pane recipe, its view and deck by the room's
  // connection, or none where the room comes from no deck; one goes where its view is not
  // hosted or no deck holds it, and a pane's name taken is numbered on.
  bench.check("Panes.cpp/Panes::place", [](Rows &rows) {
    const auto run =
        [&](std::string_view what, const Setup &setup, Params params = ide()) {
          Framed frame(std::move(params), setup.typed);
          rows.row(std::format("{} -> {}", what, frame.frames(rows, setup)));
        };
    run("a pane whose room comes from no deck",
        {.decks = {{"ide.deck", "p1,p2"}},
         .panes = {{.view = "p1"}, {.view = "p2", .deck = ""}},
         .presented = "p1"});
    run("a pane of a view no line hosts",
        {.hosted = {"p1"},
         .decks = {{"ide.deck", "p1"}},
         .panes = {{.view = "p1"}, {.view = "p2"}},
         .presented = "p1"});
    run("nodes named as p2's pane would be, of another recipe",
        {.decks = {{"ide.deck", "p1,p2"}},
         .panes = {{.view = "p1"}},
         .presented = "p1",
         .others = {"ide.p2-pane", "ide.p2-pane2"}});
    run("no keys connected",
        {.decks = {{"ide.deck", "p1,p2"}}, .presented = "p1", .typed = false});
    run("two panes for p1",
        {.decks = {{"ide.deck", "p1,p2"}},
         .panes = {{.view = "p1"}, {.view = "p1", .node = "ide.extra"}, {.view = "p2"}},
         .presented = "p1"});
    run("one room and one picture",
        {.decks = {{"ide.deck", "p1,p2"}}},
        {{"to", "ide.screen.picture"},
         {"pane", "still-image"},
         {"rooms", "image.area"},
         {"pictures", "image.picture"}});
  });

  // Each window to its pane while its deck shows it and it is not presented; the view
  // presented to the screen and its pane; connections that differ go first.
  bench.check("Panes.cpp/Panes::connect", [](Rows &rows) {
    const auto run =
        [&](std::string_view what, const Setup &setup, Params params = ide()) {
          Framed frame(std::move(params));
          rows.row(std::format("{} -> {}", what, frame.frames(rows, setup)));
        };
    const Setup two{.decks = {{"ide.deck", "p1"}, {"ide.other", "p2"}},
                    .panes = {{.view = "p1"}, {.view = "p2", .deck = "ide.other"}},
                    .presented = "p2"};
    run("two decks, p1's window not connected", two);
    Setup windowed = two;
    windowed.windows = {"p1", "p2"};
    run("two decks, a window to p2's pane while p2 is presented", windowed);
    run("p2 presented to the screen alone",
        {.decks = {{"ide.deck", "p1,p2"}},
         .panes = {{.view = "p1"}, {.view = "p2"}},
         .presented = "p2",
         .present_pane = false});
    run("no to param",
        {.decks = {{"ide.deck", "p1,p2"}}},
        {{"pane", "still-image"},
         {"rooms", "image.area,still.area"},
         {"pictures", "image.picture,still.picture"}});
    run("to a.b,c.d",
        {.decks = {{"ide.deck", "p1,p2"}}},
        {{"to", "a.b,c.d"},
         {"pane", "still-image"},
         {"rooms", "image.area"},
         {"pictures", "image.picture"}});
  });

  // Each pane's picture handed the pointer as its interact param says, a pane added
  // taking it as it comes.
  bench.check("Panes.cpp/Panes::interact", [](Rows &rows) {
    const Setup settled{.decks = {{"ide.deck", "p1,p2"}},
                        .panes = {{.view = "p1"}, {.view = "p2"}},
                        .presented = "p2"};
    Setup interacting = settled;
    interacting.interacting = true;
    Framed yes(ide("yes"));
    rows.row("yes, the pictures not interacting -> " + yes.frames(rows, settled));
    Framed kept(ide("yes"));
    rows.row("yes, the pictures interacting -> " + kept.frames(rows, interacting));
    Framed no(ide("no"));
    rows.row("no, the pictures interacting -> " + no.frames(rows, interacting));
    Framed adding(ide("yes"));
    rows.row("yes, panes to add -> " +
             adding.frames(rows, {.decks = {{"ide.deck", ""}}}));
  });
}

} // namespace VP::bench
