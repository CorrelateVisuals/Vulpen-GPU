#pragma once

#include "runtime/Operator.h"

// Divides a Rect by a tree of ratios. Each ratio is a param, so dragging a seam is a
// command and replaying the log puts every seam back.
class Split : public VP::Operator {};
