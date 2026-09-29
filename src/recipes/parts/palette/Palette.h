#pragma once

#include "runtime/Operator.h"

// Parses the theme once and publishes the palette, so the drawing parts share one look
// without sharing code.
class Palette : public VP::Operator {};
