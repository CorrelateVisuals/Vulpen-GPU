#pragma once

#include "runtime/Operator.h"

// Only Text.cpp includes this header, so the class stays in the unnamed namespace, and
// two copies of the part in one binary never clash.
namespace {

// The text a person reads and edits, loaded and saved through the file port. It writes
// Labels and Rects and draws nothing, so every text area is this part plus the drawing
// parts and a fix to text reaches them all. Find is one of its commands, so a find bar
// is only a front end on it.
class Text final : public VP::Operator {};

} // namespace
