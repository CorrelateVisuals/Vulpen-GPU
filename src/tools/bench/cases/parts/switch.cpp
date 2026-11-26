#include "switch/Switch.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"

#include <string>
#include <utility>
#include <vector>

// The library's parts/switch: the area handed whole to the half its shown param names,
// and none to the other, as a dock shows one of two panels in one place.

namespace VP::bench {

namespace domain {
// The shown param as a manifest gives it.
struct ShownWord {};
} // namespace domain

template <> struct Gen<domain::ShownWord> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"first", "second", "", "First", "both"};
  }
  static std::string random(Random &random) {
    const std::vector<std::string> all = edges();
    return all[random.below(all.size())];
  }
};

namespace {

// The first half and the second, after a frame.
std::pair<VP_VIEW::Rect, VP_VIEW::Rect> switched(const std::string &shown,
                                                 const VP_VIEW::Rect &area) {
  Running<Switch> node({{"shown", shown}});
  node.bound();
  node.port<VP_VIEW::Rect>("area") = area;
  node.cooked();
  return {node.port<VP_VIEW::Rect>("first"), node.port<VP_VIEW::Rect>("second")};
}

} // namespace

void cases(Bench &bench) {
  bench.check("Switch.cpp/Switch::bind", [](Rows &rows) {
    Running<Switch> node({{"shown", "second"}});
    node.bound();
    rows.row(show(node.bind.asked, rows.names()));
  });
  bench.sweep("Switch.cpp/Switch::cook", switched, domain::ShownWord{}, domain::Type{});
}

} // namespace VP::bench
