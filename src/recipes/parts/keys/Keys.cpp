#include "runtime/Operator.h"

namespace {

// One owner for where a key goes: a keymap chord becomes command text and any other key
// goes to the part in focus, so a key does nothing a typed command cannot.
class Keys final : public VP::Operator {};

} // namespace

VP_RECIPE(registry) {
  registry.add<Keys>("Keys");
}
