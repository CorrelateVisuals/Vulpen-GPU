#pragma once

#include "baseclasses/Mechanics.h"
#include "baseclasses/Pipelines.h"
#include "baseclasses/Resources.h"
#include "baseclasses/Swapchain.h"

namespace VP {

// The top of baseclasses: owns the GPU and runs one frame of passes. Barriers follow
// from what each pass declares it reads and writes, so nobody places them by hand.
class Engine {};
struct Pass {};

} // namespace VP
