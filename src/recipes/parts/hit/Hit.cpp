#include "runtime/Operator.h"

namespace {

// Finds the Rect under the pointer: a press sends its Item's command text and a hover
// names the Item. Connecting it makes anything drawn from Rects clickable, so no drawing
// part reads the pointer.
class Hit final : public VP::Operator {};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Hit>("Hit");
}
