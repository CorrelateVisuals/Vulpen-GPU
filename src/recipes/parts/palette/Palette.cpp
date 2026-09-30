#include "runtime/Operator.h"

namespace {

// Parses the theme once and publishes the palette, so the drawing parts share one look
// without sharing code.
class Palette final : public VP::Operator {};

} // namespace

VP_RECIPE(registry) {
  registry.add<Palette>("Palette");
}
