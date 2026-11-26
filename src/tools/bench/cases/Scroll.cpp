#include "shared/Scroll.h"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

// The library's shared/Scroll.h and Scroll.cpp: rows of which as many as fit show, from a
// first row the wheel, a drag past them and a drag of a bar's thumb move, as the editor's
// and the terminal's do. Each row is a turn of a person's on a scroll fitted to a hundred
// rows, ten shown, or another size; the track is down a 200 pixel high area.

namespace VP::bench {

struct ScrollLength {
  using type = float (::Scroll::*)(const VP_VIEW::Rect &) const;
  friend constexpr type reach(ScrollLength);
};
template struct Reach<ScrollLength, &::Scroll::length>;

namespace {

const VP_VIEW::Rect area{.offset = {10, 20}, .extent = {300, 200}};

::Scroll fitted(std::size_t rows = 100, std::size_t shown = 10, std::size_t last = 90) {
  ::Scroll scroll;
  scroll.fit(rows, shown, last);
  return scroll;
}

std::string state(const ::Scroll &scroll, Names &names) {
  return std::format("first {}{}, thumb {}",
                     scroll.first(),
                     scroll.at_end() ? " at the end" : "",
                     show(scroll.thumb(scroll.track(area)), names));
}

} // namespace

void cases(Bench &bench) {
  bench.sweep("Scroll.cpp/Scroll::fit",
              [](std::size_t rows, std::size_t shown, std::size_t last) {
                ::Scroll scroll = fitted();
                scroll.wheel(-10.0f);
                scroll.fit(rows, shown, last);
                return std::format("first {} at_end {}", scroll.first(), scroll.at_end());
              });
  bench.sweep("Scroll.cpp/Scroll::first", [](float rows) {
    ::Scroll scroll = fitted();
    scroll.move(rows);
    return scroll.first();
  });
  bench.sweep("Scroll.cpp/Scroll::at_end", [](float rows) {
    ::Scroll scroll = fitted();
    scroll.move(rows);
    return scroll.at_end();
  });
  bench.sweep("Scroll.cpp/Scroll::wheel", [](float turn) {
    ::Scroll scroll = fitted();
    scroll.move(40.0f);
    scroll.wheel(turn);
    return scroll.first();
  });
  bench.sweep("Scroll.cpp/Scroll::move", [](float first, float rows) {
    ::Scroll scroll = fitted();
    scroll.move(first);
    scroll.move(rows);
    return scroll.first();
  });
  bench.holds("Scroll.cpp/Scroll::move",
              "the first row stays from 0 to the last that may show first",
              [](std::size_t last, float rows) {
                if (std::isnan(rows)) // no caller hands it one (Ports refuses a wheel's)
                  return true;
                ::Scroll scroll = fitted(last + 10, 10, last);
                scroll.move(rows);
                return scroll.first() <= last;
              });
  bench.sweep("Scroll.cpp/Scroll::show", [](float first, std::size_t row) {
    ::Scroll scroll = fitted();
    scroll.move(first);
    scroll.show(row);
    return scroll.first();
  });
  bench.holds("Scroll.cpp/Scroll::show",
              "the row shows after, while it is a row there is",
              [](float first, std::size_t row) {
                ::Scroll scroll = fitted();
                scroll.move(first);
                scroll.show(row);
                return row >= 100 || (scroll.first() <= row && row < scroll.first() + 10);
              });
  bench.sweep("Scroll.cpp/Scroll::drift", [](float past) {
    ::Scroll scroll = fitted();
    scroll.move(40.0f);
    scroll.drift(past);
    return scroll.first();
  });

  bench.check("Scroll.cpp/Scroll::press", [](Rows &rows) {
    for (const float y : {20.0f, 25.0f, 60.0f, 119.0f, 219.0f, 220.0f, 300.0f}) {
      ::Scroll scroll = fitted();
      const bool took = scroll.press({305.0f, y}, scroll.track(area));
      rows.row(
          std::format("press at y {} -> {}; {}", y, took, state(scroll, rows.names())));
    }
    ::Scroll scroll = fitted();
    rows.row(std::format("press left of the track -> {}",
                         scroll.press({250.0f, 60.0f}, scroll.track(area))));
    ::Scroll all = fitted(5, 10, 0);
    rows.row(std::format("press while every row shows -> {}",
                         all.press({305.0f, 60.0f}, all.track(area))));
  });
  bench.check("Scroll.cpp/Scroll::drag", [](Rows &rows) {
    ::Scroll scroll = fitted();
    const VP_VIEW::Rect track = scroll.track(area);
    scroll.drag({305.0f, 120.0f}, track);
    rows.row(std::format("drag, nothing held -> {}", state(scroll, rows.names())));
    scroll.press({305.0f, 25.0f}, track);
    for (const float y : {30.0f, 120.0f, 219.0f, 400.0f, -50.0f, 25.0f}) {
      scroll.drag({305.0f, y}, track);
      rows.row(std::format("drag to y {} -> {}", y, state(scroll, rows.names())));
    }
  });
  bench.check("Scroll.cpp/Scroll::release", [](Rows &rows) {
    ::Scroll scroll = fitted();
    const VP_VIEW::Rect track = scroll.track(area);
    scroll.press({305.0f, 25.0f}, track);
    scroll.release();
    scroll.drag({305.0f, 200.0f}, track);
    rows.row(std::format("press, release, drag -> {}", state(scroll, rows.names())));
  });
  bench.sweep("Scroll.cpp/Scroll::thumb",
              [](std::size_t rows, std::size_t shown, std::size_t last, float first) {
                ::Scroll scroll;
                scroll.fit(rows, shown, last);
                scroll.move(first);
                return scroll.thumb(scroll.track(area));
              });
  bench.check("Scroll.cpp/Scroll::track", [](Rows &rows) {
    for (const VP_VIEW::Rect &along :
         {area,
          VP_VIEW::Rect{},
          VP_VIEW::Rect{.offset = {-5, 7}, .extent = {5, 5}},
          VP_VIEW::Rect{.offset = {0, 0}, .extent = {10, 10}},
          VP_VIEW::Rect{.offset = {0, 0}, .extent = {11, 3000}}})
      rows.row(
          std::format("{} -> down {}, across {}",
                      show(along, rows.names()),
                      show(::Scroll().track(along), rows.names()),
                      show(::Scroll(::Scroll::Axis::across).track(along), rows.names())));
  });
  bench.sweep(
      "Scroll.cpp/Scroll::length",
      [](std::size_t rows, std::size_t shown, std::size_t last, std::uint32_t high) {
        ::Scroll scroll;
        scroll.fit(rows, shown, last);
        const VP_VIEW::Rect track{.extent = {10, high}};
        return (scroll.*reach(ScrollLength{}))(track);
      });
  bench.sweep("Scroll.h/Scroll::Scroll", [](bool across) {
    ::Scroll scroll(across ? ::Scroll::Axis::across : ::Scroll::Axis::down);
    scroll.fit(100, 10, 90);
    scroll.move(45.0f);
    return scroll.thumb(scroll.track(area));
  });
}

} // namespace VP::bench
