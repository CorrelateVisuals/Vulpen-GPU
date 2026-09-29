#pragma once

#include "baseclasses/Log.h"

#include <vulkan/vulkan.h>

namespace VP {

// The GPU machine: instance, device and queues. It knows no window, so a headless run
// needs nothing else.
class Mechanics {};

} // namespace VP
