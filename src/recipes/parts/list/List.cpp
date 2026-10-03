#include "runtime/Operator.h"

namespace {

// Lays out Items as a row or a column of cells, in its Rect or floated at an anchor. A
// menubar, a context menu, a tab strip and a completion popup are this part with other
// Items, so none of them places cells itself.
class List final : public VP::Operator {};

} // namespace

VP_OPERATORS(registry) {
  registry.add<List>("List");
}
