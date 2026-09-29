#pragma once

#include "baseclasses/Engine.h"
#include "runtime/Operator.h"
#include "runtime/View.h"

namespace VP {

// Where the graph meets the GPU: runs each node's operator in graph order and turns
// the view into passes that declare what they read and write.
class Schedule {};

} // namespace VP
