#include "bar/Bar.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The library's parts/bar: a strip along one edge of the area, sized in font cells, and
// the rest of the area beside it, a seam between where asked, as a dock lays out a menu
// bar or a status line.

namespace VP::bench {

namespace domain {
// The edge param as a manifest gives it.
struct EdgeWord {};
// The seam param as a manifest gives it.
struct SeamWord {};
} // namespace domain

template <> struct Gen<domain::EdgeWord> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"top", "bottom", "left", "right", "", "Top"};
  }
  static std::string random(Random &random) {
    const std::vector<std::string> all = edges();
    return all[random.below(all.size())];
  }
};

template <> struct Gen<domain::SeamWord> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"no", "yes", "", "maybe"};
  }
  static std::string random(Random &random) {
    const std::vector<std::string> all = edges();
    return all[random.below(all.size())];
  }
};

namespace {

// The strip and the rest, after a frame.
std::pair<VP_VIEW::Rect, VP_VIEW::Rect> barred(const std::string &edge,
                                               std::uint32_t size,
                                               const std::string &seam,
                                               const VP_VIEW::Rect &area,
                                               const VP_VIEW::Metrics &font) {
  Running<Bar> node({{"edge", edge}, {"size", std::to_string(size)}, {"seam", seam}});
  node.bound();
  node.port<VP_VIEW::Rect>("area") = area;
  node.port<VP_VIEW::Metrics>("font") = font;
  node.cooked();
  return {node.port<VP_VIEW::Rect>("strip"), node.port<VP_VIEW::Rect>("rest")};
}

} // namespace

void cases(Bench &bench) {
  bench.check("Bar.cpp/Bar::bind", [](Rows &rows) {
    Running<Bar> node({{"edge", "top"}, {"size", "1"}, {"seam", "no"}});
    node.bound();
    rows.row(show(node.bind.asked, rows.names()));
  });
  bench.sweep("Bar.cpp/Bar::cook",
              barred,
              domain::EdgeWord{},
              domain::Type{},
              domain::SeamWord{},
              domain::Type{},
              domain::Type{});
}

} // namespace VP::bench
