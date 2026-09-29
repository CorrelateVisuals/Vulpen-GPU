#pragma once

#include "runtime/View.h"

namespace VP {

// Every change to a view is a command through this one port, so the CLI, a GUI, a
// script and an agent act alike, and replaying the log rebuilds the session.
class CommandPort {};
struct Command {};
class CommandLog {};

} // namespace VP
