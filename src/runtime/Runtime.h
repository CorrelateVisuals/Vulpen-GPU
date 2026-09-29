#pragma once

#include "baseclasses/Engine.h"
#include "baseclasses/Platform.h"
#include "runtime/Commands.h"
#include "runtime/Manifest.h"
#include "runtime/Recipes.h"
#include "runtime/Schedule.h"
#include "runtime/View.h"

namespace VP {

// The top of the tree: owns every module and runs the loop, so main() only builds one
// and a test can build one without a window.
class Runtime {};

} // namespace VP
