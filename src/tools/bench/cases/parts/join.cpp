#include "join/Join.cpp"

#include "tools/bench/Bench.h"
#include "tools/bench/Contracts.h"
#include "tools/bench/Fakes.h"

// The library's parts/join: two lists of items as one, a divider between when both hold
// some, as a menu joins its own lines to those of what it was opened on.

namespace VP::bench {

namespace {

// The items the two lists join to, after a frame with the second as it was first; a
// second that changed is joined anew, one that did not keeps the frame's join.
VP_VIEW::Items joined(const VP_VIEW::Items &first,
                      const VP_VIEW::Items &earlier,
                      const VP_VIEW::Items &second) {
  Running<Join> node;
  node.bound();
  node.port<VP_VIEW::Items>("first") = first;
  node.port<VP_VIEW::Items>("second") = earlier;
  node.cooked();
  node.port<VP_VIEW::Items>("second") = second;
  node.cooked();
  return node.port<VP_VIEW::Items>("items");
}

} // namespace

void cases(Bench &bench) {
  bench.check("Join.cpp/Join::bind", [](Rows &rows) {
    Running<Join> node;
    node.bound();
    rows.row(show(node.bind.asked, rows.names()));
  });
  bench.sweep("Join.cpp/Join::cook", joined);
}

} // namespace VP::bench
