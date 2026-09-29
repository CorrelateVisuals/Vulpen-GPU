#pragma once

#include "runtime/Operator.h"

// Which node reaches the screen is a manifest fact; a mode switch is the command that
// changes it, so it replays like any edit.
class Modes : public VP::Operator {};
