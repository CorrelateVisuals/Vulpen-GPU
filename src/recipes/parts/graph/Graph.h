#pragma once

#include "runtime/Operator.h"

// The view's graph on a pannable canvas: nodes as Rects and Labels, connections and
// relations as Curves. It edits the graph only through commands, so a drag on the canvas
// is an edit the log replays.
class Graph : public VP::Operator {};
