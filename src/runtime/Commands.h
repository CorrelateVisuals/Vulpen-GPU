#pragma once

#include "runtime/Operator.h"

#include <string_view>

namespace VP {

// Finds the view a command addresses by name (`triangle: node add …`). The views' owner
// implements it, so the port reaches a view without including its owner.
class ViewLookup {
public:
  // Null when no view has the name.
  virtual View *find(std::string_view name) = 0;

protected:
  ~ViewLookup() = default;
};

// Every change to a view is a command through this one port, so the CLI, a GUI, a
// script and an agent act alike, and replaying the log rebuilds the session. A command
// registers with its usage, whose placeholders give its completion, and its help, or
// not at all (RV04).
class Commands final : public CommandPort, public CommandHandler {
  void command(Call &call) override;
};

// The session as groups: each line keeps the primitive commands it expanded to, so a
// replay needs no recipe (V08) and undo removes one group.
class CommandLog {};

} // namespace VP
