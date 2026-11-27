#include "list/List.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The library's parts/list: the Items it is handed laid out in its place, a row each in a
// framed column, or side by side as tabs, or in a column dropped below or right of its
// place, as a menu; where each shows, its close mark and the current one handed on, and
// the rects and labels its rects and glyphs nodes draw. A strip of tabs scrolls by the
// wheel and to the current tab, shows a long label whole below its tab while the pointer
// is over it, and flashes a stuck tab pressed.

namespace VP::bench {

namespace {

using Params = std::vector<std::pair<std::string, std::string>>;

std::string role_text(std::uint32_t role) {
  return role < VP_VIEW::role_names.size() ? std::string(VP_VIEW::role_names[role])
                                           : std::format("role {}", role);
}

std::string rect_text(const VP_VIEW::Rect &rect, Names &names) {
  std::string text = std::format("{} {}", show(rect, names), role_text(rect.role));
  if (rect.blend != 0.0f)
    text += std::format(
        " blending {} {}", role_text(rect.blend_role), number_text(rect.blend));
  return text;
}

std::string rects_text(const VP_VIEW::Rects &rects, Names &names) {
  std::string text;
  for (const VP_VIEW::Rect &rect : rects)
    text += (text.empty() ? "" : ", ") + show(rect, names);
  return "[" + text + "]";
}

// The node, its window 400x300 and its font's cell 8x16 unless a row says otherwise.
struct Framed {
  Running<List> node;
  FakeInput input;

  explicit Framed(Params params) : node(std::move(params)) {
    node.bound();
    node.cook.input_port = &input;
    node.cook.size = {400, 300};
    node.port<VP_VIEW::Metrics>("font").font.cell = {8, 16};
  }
  void hand(VP_VIEW::Items items, VP_VIEW::Rect place) {
    node.port<VP_VIEW::Items>("items") = std::move(items);
    node.port<VP_VIEW::Rect>("place") = place;
  }
  // A frame of these events: where each Item shows, its mark, the current one, and what
  // its rects and glyphs draw.
  std::string cooked(Rows &rows, std::vector<Event> events = {}) {
    input.given = std::move(events);
    node.cooked();
    Names &names = rows.names();
    std::string text = std::format("shown {}; marks {}; current {}; rects",
                                   rects_text(node.port<VP_VIEW::Rects>("shown"), names),
                                   rects_text(node.port<VP_VIEW::Rects>("marks"), names),
                                   show(node.port<VP_VIEW::Rect>("current"), names));
    const FakeCook &cook = node.cook;
    const auto *const rects =
        reinterpret_cast<const VP_VIEW::Rect *>(cook.written[0].data());
    for (std::size_t at = 0; at < cook.used[0]; ++at)
      text += " " + rect_text(rects[at], names) + ";";
    text += " labels";
    const auto *const labels =
        reinterpret_cast<const VP_VIEW::Label *>(cook.written[1].data());
    const auto *const characters =
        reinterpret_cast<const VP_VIEW::Character *>(cook.written[2].data());
    for (std::size_t at = 0; at < cook.used[1]; ++at) {
      const VP_VIEW::Label &label = labels[at];
      std::string said;
      for (std::uint32_t code = 0; code < label.count; ++code)
        said += static_cast<char>(characters[label.first + code].code);
      text += std::format(" {}+{} {} {}",
                          show(label.offset, names),
                          show(label.extent, names),
                          role_text(label.role),
                          escaped(said));
      if (label.cut != glm::uvec2(0))
        text += " cut " + show(label.cut, names);
      text += ";";
    }
    return text + std::format(" {} characters", cook.used[2]);
  }
};

Params column(std::string drop = "no") {
  return {{"axis", "y"}, {"drop", std::move(drop)}, {"end", "0"}};
}
Params strip(std::string end = "0") {
  return {{"axis", "x"}, {"drop", "no"}, {"end", std::move(end)}};
}

VP_VIEW::Item item(std::string label) {
  return {.label = std::move(label), .command = "c"};
}

Event pointer(glm::vec2 at) {
  return {.kind = Event::Kind::pointer, .at = at};
}
Event press() {
  return {.kind = Event::Kind::button, .down = true, .name = "left", .presses = 1};
}
Event wheel(glm::vec2 turn) {
  return {.kind = Event::Kind::wheel, .turn = turn};
}

} // namespace

void cases(Bench &bench) {
  // Grown by what the part grows a Rect by, none, its margin and its frame: a Rect's
  // offset from a layout is never near the ends of an int32.
  bench.check("List.cpp/grown", [](Rows &rows) {
    for (const VP_VIEW::Rect &rect : Gen<VP_VIEW::Rect>::edges())
      for (const std::int32_t by : {0, margin, margin + frame_width})
        rows.row(std::format(
            "{} by {} -> {}",
            show(rect, rows.names()),
            by,
            rect_text(grown(rect, by, VP_VIEW::role("panel")), rows.names())));
  });
  bench.sweep("List.cpp/right_of", right_of, domain::Type{});
  // The part's own, not the engine's VP::within of node paths.
  bench.sweep("List.cpp/within", ::within, domain::Type{}, domain::Type{});

  // Its params, a word each from a closed set, and what it asks for.
  bench.check("List.cpp/List::bind", [](Rows &rows) {
    for (const Params &params : {column(),
                                 strip("2"),
                                 column("below"),
                                 column("right"),
                                 Params{{"axis", "z"}, {"drop", "no"}, {"end", "0"}},
                                 Params{{"axis", "y"}, {"drop", "up"}, {"end", "0"}},
                                 Params{{"axis", "y"}, {"drop", "no"}, {"end", "-1"}}}) {
      std::string said;
      try {
        Running<List> node(params);
        node.bound();
        said = show(node.bind.asked, rows.names());
      } catch (...) {
        said = thrown();
      }
      rows.row(std::format("{} -> {}", show(params, rows.names()), said));
    }
  });

  // A row each from the place's top, as many as its height holds, up to 64, in a frame
  // and a fill; a divider a rule across its row; the current Item's row marked; a label
  // cut at its row's end; nothing drawn with no room, no Items or no cell.
  bench.check("List.cpp/List::stacked", [](Rows &rows) {
    const VP_VIEW::Items items{item("open"),
                               {},
                               {.label = "save", .current = true},
                               {.label = "a label longer than its row"},
                               item("quit")};
    const auto run = [&](std::string_view what,
                         const VP_VIEW::Items &given,
                         VP_VIEW::Rect place,
                         glm::uvec2 cell = {8, 16}) {
      Framed frame(column());
      frame.hand(given, place);
      frame.node.port<VP_VIEW::Metrics>("font").font.cell = cell;
      rows.row(std::format("{} -> {}", what, frame.cooked(rows)));
    };
    run("five Items, room for three rows",
        items,
        {.offset = {10, 20}, .extent = {80, 48}});
    run("five Items, room for all", items, {.offset = {10, 20}, .extent = {80, 200}});
    run("no Items", {}, {.offset = {10, 20}, .extent = {80, 200}});
    run("a place of no height", items, {.offset = {10, 20}, .extent = {80, 0}});
    run("a place lower than a row", items, {.offset = {10, 20}, .extent = {80, 15}});
    run("a cell of no width", items, {.offset = {10, 20}, .extent = {80, 200}}, {0, 16});
    run("a label in the role of a number",
        {{.label = "7", .role = VP_VIEW::role("number")}},
        {.offset = {0, 0}, .extent = {80, 16}});
    Framed many(column());
    many.hand(VP_VIEW::Items(70, item("x")), {.offset = {0, 0}, .extent = {8, 70 * 16}});
    many.cooked(rows);
    rows.row(std::format("70 Items, room for all -> {} shown, {} rects",
                         many.node.port<VP_VIEW::Rects>("shown").size(),
                         many.node.cook.used[0]));
  });

  // Dropped below its place, from its left edge, as wide as the widest label or the
  // place; or right of it, level with its top, its fill over the shared border, or left
  // where the window has no room right; moved in to stay inside the window; none while
  // the place is empty, as a menu shut.
  bench.check("List.cpp/List::dropped", [](Rows &rows) {
    const VP_VIEW::Items items{item("new"), item("open a file"), {}, item("quit")};
    const auto run = [&](std::string_view drop,
                         std::string_view what,
                         VP_VIEW::Rect place,
                         const VP_VIEW::Items &given) {
      Framed frame(column(std::string(drop)));
      frame.hand(given, place);
      rows.row(std::format("{}, {} -> {}", drop, what, frame.cooked(rows)));
    };
    run("below", "under a tab", {.offset = {20, 0}, .extent = {40, 20}}, items);
    run("below", "under a wide place", {.offset = {20, 0}, .extent = {200, 20}}, items);
    run("below",
        "at the window's right edge",
        {.offset = {380, 0}, .extent = {20, 20}},
        items);
    run("below",
        "at the window's bottom",
        {.offset = {20, 290}, .extent = {40, 10}},
        items);
    run("below", "shut", {.offset = {20, 0}, .extent = {0, 20}}, items);
    run("below", "no Items", {.offset = {20, 0}, .extent = {40, 20}}, {});
    run("right", "beside a row", {.offset = {20, 40}, .extent = {80, 16}}, items);
    run("right", "no room right", {.offset = {300, 40}, .extent = {80, 16}}, items);
    run("right", "low in the window", {.offset = {20, 290}, .extent = {80, 16}}, items);
    run("right", "shut", {}, items);
    Framed tall(column("right"));
    tall.hand(VP_VIEW::Items(30, item("x")), {.offset = {20, 40}, .extent = {80, 16}});
    rows.row("right, taller than the window -> " + tall.cooked(rows));
  });

  // Side by side: each tab as wide as its label, up to 24 cells, and its close mark, with
  // a cell either side; toggles joined with no gap, a joined Item's label right after the
  // one before; a divider a line in a cell of its own; the strip its place but its end
  // cells; the label cut at its start past 24 cells.
  bench.check("List.cpp/List::side_by_side", [](Rows &rows) {
    const auto run = [&](std::string_view what,
                         const Params &params,
                         const VP_VIEW::Items &given,
                         VP_VIEW::Rect place) {
      Framed frame(params);
      frame.hand(given, place);
      rows.row(std::format("{} -> {}", what, frame.cooked(rows)));
    };
    const VP_VIEW::Rect bar{.offset = {0, 0}, .extent = {400, 20}};
    run("two tabs, the second current with a mark",
        strip(),
        {item("one"), {.label = "two", .current = true, .close = "close two"}},
        bar);
    run("edit and run toggling, a divider, gpu",
        strip(),
        {{.label = "edit", .current = true, .toggles = true},
         {.label = "run", .toggles = true},
         {},
         {.label = "gpu",
          .current = true,
          .toggles = true,
          .role = VP_VIEW::role("number")}},
        bar);
    run("a joined Item",
        strip(),
        {item("view"), {.label = ".vlp", .joined = true}, item("next")},
        bar);
    run("a label of 30 letters", strip(), {item("abcdefghijklmnopqrstuvwxyz1234")}, bar);
    run("a path of 34 letters, the pointer past it",
        strip(),
        {item("projects/triangles/triangle-params")},
        {.offset = {0, 40}, .extent = {400, 20}});
    run("a strip lower than a cell", strip(), {item("one")}, {.extent = {400, 15}});
    run("end 10, the second tab's mark past the strip",
        strip("10"),
        {item("abcdefghijklmnopqrstu"), {.label = "x", .close = "close x"}},
        {.offset = {0, 0}, .extent = {280, 20}});
    run("end past the place", strip("100"), {item("one")}, bar);
    run("a muted button with a role, off and on",
        strip(),
        {{.label = "7 fps", .role = VP_VIEW::role("muted")},
         {.label = "lit", .current = true, .role = VP_VIEW::role("number")}},
        bar);
    Framed many(strip());
    many.hand(VP_VIEW::Items(70, item("x")), bar);
    many.cooked(rows);
    rows.row(std::format("70 tabs -> {} shown, {} rects",
                         many.node.port<VP_VIEW::Rects>("shown").size(),
                         many.node.cook.used[0]));
  });

  // Whether an Item goes on from the one before as one tab: never the first, nor one
  // past the end.
  bench.check("List.cpp/List::joins", [](Rows &rows) {
    Framed frame(strip());
    frame.hand(
        {{.label = "a", .joined = true}, {.label = "b", .joined = true}, item("c")},
        {.extent = {400, 20}});
    frame.cooked(rows);
    rows.row("a joined first, b joined, c not -> " + frame.cooked(rows));
  });

  // The strip scrolls by the wheel over it, either way, and to the current tab as it
  // changes, or as their count does, never past the first tab or the last.
  bench.check("List.cpp/List::scroll", [](Rows &rows) {
    VP_VIEW::Items tabs;
    for (int at = 0; at < 8; ++at)
      tabs.push_back(item(std::format("tab{}", at)));
    Framed frame(strip());
    frame.hand(tabs, {.offset = {0, 0}, .extent = {200, 20}});
    // The first tab and the last, where the strip shows them.
    const auto shown = [&](std::vector<Event> events) {
      frame.cooked(rows, std::move(events));
      const VP_VIEW::Rects &laid = frame.node.port<VP_VIEW::Rects>("shown");
      return std::format("first {}, last {}",
                         show(laid.front(), rows.names()),
                         show(laid.back(), rows.names()));
    };
    rows.row("at the start -> " + shown({}));
    rows.row("the wheel down over the strip -> " +
             shown({pointer({10, 10}), wheel({0, -1})}));
    rows.row("again, across -> " + shown({wheel({-1, 0})}));
    rows.row("far past the end -> " + shown({wheel({0, -100})}));
    rows.row("the pointer below the strip, the wheel up -> " +
             shown({pointer({10, 30}), wheel({0, 100})}));
    rows.row("over it again, far past the start -> " +
             shown({pointer({10, 10}), wheel({0, 100})}));
    tabs.back().current = true;
    frame.hand(tabs, {.offset = {0, 0}, .extent = {200, 20}});
    rows.row("the last current -> " + shown({}));
    rows.row("the wheel up, the last still current -> " + shown({wheel({0, 1})}));
    tabs.back().current = false;
    tabs.front().current = true;
    frame.hand(tabs, {.offset = {0, 0}, .extent = {200, 20}});
    rows.row("the first current -> " + shown({}));
    tabs.resize(2);
    frame.hand(tabs, {.offset = {0, 0}, .extent = {200, 20}});
    rows.row("two tabs left -> " + shown({}));
  });

  // The pointer over a tab whose label is cut shows the label whole below it, in a frame
  // and a fill, moved in to stay inside the place and the window; over another, none.
  bench.check("List.cpp/List::tip_row", [](Rows &rows) {
    const VP_VIEW::Items tabs{item("short"), item("a label that runs past 24 cells")};
    const auto run = [&](std::string_view what, VP_VIEW::Rect place, glm::vec2 at) {
      Framed frame(strip());
      frame.hand(tabs, place);
      rows.row(std::format("{} -> {}", what, frame.cooked(rows, {pointer(at)})));
    };
    run("over the long one", {.offset = {0, 0}, .extent = {400, 20}}, {100, 10});
    run("over the short one", {.offset = {0, 0}, .extent = {400, 20}}, {10, 10});
    run("over the long one, the place narrower than its label",
        {.offset = {0, 0}, .extent = {250, 20}},
        {100, 10});
    run("past the strip", {.offset = {0, 0}, .extent = {400, 20}}, {100, 40});
  });

  // A stuck tab pressed flashes in the error's color, fading back over 30 frames; a tab
  // not stuck, or a press past it, does not.
  bench.check("List.cpp/List::back", [](Rows &rows) {
    Framed frame(strip());
    frame.hand({{.label = "run", .stuck = true}, item("gpu")}, {.extent = {400, 20}});
    const auto flash = [&](std::uint64_t index, std::vector<Event> events) {
      frame.node.cook.frame = index;
      frame.cooked(rows, std::move(events));
      const auto *const rects =
          reinterpret_cast<const VP_VIEW::Rect *>(frame.node.cook.written[0].data());
      return rect_text(rects[1], rows.names());
    };
    rows.row("frame 0, a press on gpu -> run " + flash(0, {pointer({60, 10}), press()}));
    rows.row("frame 1, a press on run -> run " + flash(1, {pointer({10, 10}), press()}));
    rows.row("frame 16 -> run " + flash(16, {}));
    rows.row("frame 30 -> run " + flash(30, {}));
    rows.row("frame 31 -> run " + flash(31, {}));
    rows.row("frame 40, a press below it -> run " +
             flash(40, {pointer({10, 30}), press()}));
    rows.row("frame 41, a press as the pointer moves onto it, then off -> run " +
             flash(41, {pointer({10, 10}), press(), pointer({10, 30})}));
  });

  // A tab's label a cell in from its left, cut where the strip cuts its tab, a character
  // at a time, with the cut's pixels in its first cell.
  bench.check("List.cpp/List::tab_label", [](Rows &rows) {
    VP_VIEW::Items tabs;
    for (int at = 0; at < 6; ++at)
      tabs.push_back(item(std::format("label{}", at)));
    for (const float turn : {0.0f, -0.3f, -1.0f}) {
      Framed frame(strip());
      frame.hand(tabs, {.offset = {0, 0}, .extent = {150, 20}});
      frame.cooked(rows, {pointer({10, 10}), wheel({0, turn})});
      rows.row(std::format(
          "the wheel turned {} -> {}", number_text(turn), frame.cooked(rows)));
    }
  });
}

} // namespace VP::bench
