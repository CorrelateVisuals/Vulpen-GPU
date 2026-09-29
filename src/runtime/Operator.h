#pragma once

#include "baseclasses/Log.h"
#include "runtime/Commands.h"

namespace VP {

// The one runtime header a recipe's C++ includes: a node's behaviour and the general
// ports (input, commands, files). Nothing here reaches Vulkan or the OS.
class Operator {};
class InputPort {};
class FilePort {};

} // namespace VP
