#pragma once

#include "runtime/Commands.h"
#include "runtime/Manifest.h"
#include "runtime/View.h"

namespace VP {

// The recipe library. Deploying copies a recipe into the view, which owns that copy
// from then on; a later library edit never reaches it.
class Recipes {};

} // namespace VP
