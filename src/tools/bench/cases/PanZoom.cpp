#include "shared/PanZoom.h"

#include "tools/bench/Bench.h"

#include <glm/geometric.hpp>

#include <cmath>
#include <format>
#include <string>
#include <tuple>
#include <utility>

// The library's shared/PanZoom.h: how far what an area shows moved, and how near it is,
// so the graph and the Perform panel's image move alike. Each row starts from a pan and a
// zoom a person may have reached.

namespace VP::bench {

template <> struct Shown<::PanZoom> {
  static std::string text(const ::PanZoom &view, Names &names) {
    return std::format("pan {} zoom {}", show(view.pan, names), number_text(view.zoom));
  }
};

namespace {

// Where a person may have left it: moved and zoomed in.
::PanZoom moved() {
  return {.pan = {30.0f, -12.5f}, .zoom = 1.5f};
}

bool near(glm::vec2 a, glm::vec2 b) {
  return glm::length(a - b) <= 1e-3f * (1.0f + glm::length(a) + glm::length(b));
}

} // namespace

void cases(Bench &bench) {
  bench.check("PanZoom.h/PanZoom::home", [](Rows &rows) {
    ::PanZoom view = moved();
    view.home();
    rows.row(std::format(
        "{} home -> {}", show(moved(), rows.names()), show(view, rows.names())));
  });

  bench.sweep("PanZoom.h/PanZoom::wheel", [](float turn, glm::vec2 about) {
    ::PanZoom view = moved();
    view.wheel(turn, about);
    return view;
  });
  // Past least and most as their callers go: the graph further out, to go up a level
  // there, and the image nearer, to show its pixels. Each keeps farthest below nearest,
  // as std::clamp asks.
  bench.check("PanZoom.h/PanZoom::wheel", [](Rows &rows) {
    for (const auto &[caller, farthest, nearest] :
         {std::tuple{"the graph's", 0.05f, ::PanZoom::most},
          std::tuple{"the image's", ::PanZoom::least, 10.0f * ::PanZoom::most}})
      for (const float turn : {1.0f, -1.0f, 10.0f, -10.0f, 100.0f, -100.0f}) {
        ::PanZoom view = moved();
        view.wheel(turn, {50.0f, 40.0f}, farthest, nearest);
        rows.row(std::format("{} turn {} about (50,40) -> {}",
                             caller,
                             number_text(turn),
                             show(view, rows.names())));
      }
  });
  bench.holds("PanZoom.h/PanZoom::wheel",
              "the place under the pointer stays under it, the zoom from least to most",
              [](float turn, glm::vec2 about) {
                if (!std::isfinite(turn) || !std::isfinite(about.x) ||
                    !std::isfinite(about.y) || std::abs(turn) > 100.0f ||
                    glm::length(about) > 1e6f)
                  return true;
                ::PanZoom view = moved();
                const glm::vec2 place = (about - view.pan) / view.zoom;
                view.wheel(turn, about);
                return near(view.on_screen(place), about) &&
                       view.zoom >= ::PanZoom::least && view.zoom <= ::PanZoom::most;
              });

  bench.sweep("PanZoom.h/PanZoom::on_screen",
              [](glm::vec2 at) { return moved().on_screen(at); });

  bench.sweep("PanZoom.h/PanZoom::shows",
              [](glm::vec2 room, glm::vec2 size) { return moved().shows(room, size); });
  bench.holds("PanZoom.h/PanZoom::shows",
              "unmoved, a picture fits its room at its aspect, centered",
              [](glm::vec2 room, glm::vec2 size) {
                if (!(room.x > 0.0f && room.y > 0.0f && size.x > 0.0f && size.y > 0.0f) ||
                    room.x > 1e6f || room.y > 1e6f || size.x > 1e6f || size.y > 1e6f ||
                    size.x < 1e-3f || size.y < 1e-3f)
                  return true;
                const auto [top_left, reach] = ::PanZoom{}.shows(room, size);
                const bool fits = reach.x <= room.x * 1.0001f &&
                                  reach.y <= room.y * 1.0001f &&
                                  (near({reach.x, 0.0f}, {room.x, 0.0f}) ||
                                   near({0.0f, reach.y}, {0.0f, room.y}));
                return fits && near(top_left + reach / 2.0f, room / 2.0f) &&
                       std::abs(reach.x * size.y - reach.y * size.x) <=
                           1e-3f * reach.x * size.y;
              });
}

} // namespace VP::bench
