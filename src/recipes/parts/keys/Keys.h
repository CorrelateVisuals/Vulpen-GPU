#pragma once

#include "runtime/Operator.h"

// One owner for where a key goes: a keymap chord becomes command text and any other key
// goes to the part in focus, so a key does nothing a typed command cannot.
class Keys : public VP::Operator {};
