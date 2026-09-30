#pragma once

#include "runtime/Operator.h"

// Only Graph.cpp includes this header: a recipe's C++ is one translation unit, so the
// class stays in the unnamed namespace and two views' copies of the part never clash.
namespace {

// The view's graph on a pannable canvas: nodes as Rects and Labels, connections and
// relations as Curves. It edits the graph only through commands, so a drag on the canvas
// is an edit the log replays.
class Graph final : public VP::Operator {};

} // namespace
