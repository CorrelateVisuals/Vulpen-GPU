#include "runtime/Operator.h"

namespace {

// Which node reaches the screen is a manifest fact; a mode switch is the command that
// changes it, so it replays like any edit.
class Modes final : public VP::Operator {};

} // namespace

VP_RECIPE(registry) {
  registry.add<Modes>("Modes");
}
