#include "shared/Costs.h"

#include "tools/bench/Bench.h"

#include <format>
#include <string>
#include <string_view>
#include <vector>

// The library's shared/Costs.h: what nodes cost together, which probe stats and the
// graph's heat sum alike, and which of their problems a box shows: an error before a
// warning before a wait.

namespace VP::bench {

template <> struct Shown<::Weight> {
  static std::string text(const ::Weight &weight, Names &) {
    return std::format("cpu {} ({} last) gpu {} ({} last){}{}{} problem {}{}{}",
                       number_text(weight.cpu),
                       number_text(weight.cpu_last),
                       number_text(weight.gpu),
                       number_text(weight.gpu_last),
                       weight.cooks ? " cooks" : "",
                       weight.runs ? " runs" : "",
                       weight.untimed ? " untimed" : "",
                       escaped(weight.problem),
                       weight.waits ? " waits" : "",
                       weight.warns ? " warns" : "");
  }
};

namespace {

// Nodes as a frame finds them: cooking, running timed or not, and left out for each
// reason, with times a frame may give them.
std::vector<VP::Cost> nodes() {
  return {{.node = "a", .cpu = 0.5f, .cpu_last = 0.75f, .cooks = true},
          {.node = "b", .gpu = 0.25f, .gpu_last = 0.5f, .runs = true, .timed = true},
          {.node = "c", .gpu = 2.0f, .gpu_last = 2.0f, .runs = true},
          {.node = "d", .cpu = 1.0f, .cpu_last = 1.0f, .cooks = false},
          {.node = "e", .problem = "waits for the live build", .waits = true},
          {.node = "f", .problem = "draws nothing yet", .warns = true},
          {.node = "g", .problem = "no such port"},
          {.node = "h", .problem = "waits too", .waits = true, .warns = true},
          {.node = "i",
           .cpu = 0.125f,
           .gpu = 1.0f,
           .cooks = true,
           .runs = true,
           .timed = true}};
}

} // namespace

void cases(Bench &bench) {
  bench.sweep("Costs.h/weight_of", weight_of);
  bench.check("Costs.h/weight_of", [](Rows &rows) {
    rows.row(std::format("an error over a warning over a wait over none: {}",
                         weight_of("e", false, false) > weight_of("w", false, true) &&
                             weight_of("w", false, true) > weight_of("t", true, false) &&
                             weight_of("t", true, false) > weight_of("", false, false)));
  });

  bench.check("Costs.h/add", [](Rows &rows) {
    const std::vector<VP::Cost> all = nodes();
    for (const VP::Cost &cost : all) {
      ::Weight alone;
      add(alone, cost);
      rows.row(std::format("{} alone -> {}", cost.node, show(alone, rows.names())));
    }
    ::Weight together;
    std::string added;
    for (const VP::Cost &cost : all) {
      add(together, cost);
      added.append(cost.node);
      rows.row(std::format("{} -> {}", added, show(together, rows.names())));
    }
    ::Weight backwards;
    for (auto cost = all.rbegin(); cost != all.rend(); ++cost)
      add(backwards, *cost);
    rows.row(std::format("all, last to first -> {}", show(backwards, rows.names())));
  });
}

} // namespace VP::bench
