#pragma once

#include "runtime/Commands.h"
#include "runtime/View.h"

namespace VP {

// The .vlp text of a view, both ways. Load, save and migrate are its own commands, so
// they land in the command log like any edit.
class Manifest {};

} // namespace VP
