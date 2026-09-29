#pragma once

#include "runtime/Commands.h"
#include "runtime/Manifest.h"
#include "runtime/View.h"

namespace VP {

// The recipe library. Deploying copies a recipe into the view with every recipe it
// deploys and every contract it includes, and the view owns those copies from then on;
// a later library edit never reaches them.
class Recipes {};

} // namespace VP
