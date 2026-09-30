#include "runtime/Operator.h"

namespace {

// Divides a Rect by a tree of ratios. Each ratio is a param, so dragging a seam is a
// command and replaying the log puts every seam back.
class Split final : public VP::Operator {};

} // namespace

VP_RECIPE(registry) {
  registry.add<Split>("Split");
}
