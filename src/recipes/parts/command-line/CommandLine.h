#pragma once

#include "runtime/Operator.h"

// One line of input with history and completion, sent to the command port. The CLI, the
// terminal and the find bar are this part, so each reaches Vulpen only through the
// command port.
class CommandLine : public VP::Operator {};
