#pragma once

#include "runtime/Operator.h"

namespace VP {

// Every change to a view is a command through this one port, so the CLI, a GUI, a
// script and an agent act alike, and replaying the log rebuilds the session. A command
// registers with its usage, whose placeholders give its completion, and its help, or
// not at all (RV04).
class Commands final : public CommandPort {};

// The session as groups: each line keeps the primitive commands it expanded to, so a
// replay needs no recipe (V08) and undo removes one group.
class CommandLog {};

} // namespace VP
