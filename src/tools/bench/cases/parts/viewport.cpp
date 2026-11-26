#include "viewport/Viewport.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"

#include <glm/vec2.hpp>

// The library's parts/viewport: the whole Rect a frame draws into, which a dock divides.

namespace VP::bench {

namespace {

// The area a frame of that size hands on.
VP_VIEW::Rect cooked(glm::uvec2 size) {
  Running<Viewport> node;
  node.bound();
  node.cook.size = size;
  node.cooked();
  return node.port<VP_VIEW::Rect>("area");
}

} // namespace

void cases(Bench &bench) {
  bench.check("Viewport.cpp/Viewport::bind", [](Rows &rows) {
    Running<Viewport> node;
    node.bound();
    rows.row(show(node.bind.asked, rows.names()));
  });
  bench.sweep("Viewport.cpp/Viewport::cook", cooked);
}

} // namespace VP::bench
