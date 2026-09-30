#include "runtime/Operator.h"

#include <stb_truetype.h>

namespace {

// Builds the glyph atlas once, so every glyphs part borrows one atlas instead of loading
// the font again.
class Font final : public VP::Operator {};

} // namespace

VP_RECIPE(registry) {
  registry.add<Font>("Font");
}
