#pragma once

#include "runtime/Operator.h"

// Builds the glyph atlas once, so every glyphs part borrows one atlas instead of loading
// the font again.
class Font : public VP::Operator {};
