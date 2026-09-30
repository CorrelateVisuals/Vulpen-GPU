#include "runtime/Operator.h"

namespace {

// One line of input with history and completion, sent to the command port. The CLI, the
// terminal and the find bar are this part, so each reaches Vulpen only through the
// command port.
class CommandLine final : public VP::Operator {};

} // namespace

VP_RECIPE(registry) {
  registry.add<CommandLine>("CommandLine");
}
