#pragma once

#include "runtime/Operator.h"

// Only Graph.cpp includes this header, so the class stays in the unnamed namespace, and
// two copies of the part in one binary never clash.
namespace {

// The view's graph on a pannable canvas: nodes as Rects and Labels, connections and
// relations as Curves. It edits the graph only through commands, so a drag on the canvas
// is an edit the log replays.
class Graph final : public VP::Operator {};

} // namespace
