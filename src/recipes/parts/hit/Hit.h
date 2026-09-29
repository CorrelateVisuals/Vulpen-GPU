#pragma once

#include "runtime/Operator.h"

// Finds the Rect under the pointer: a press sends its Item's command text and a hover
// names the Item. Connecting it makes anything drawn from Rects clickable, so no drawing
// part reads the pointer.
class Hit : public VP::Operator {};
