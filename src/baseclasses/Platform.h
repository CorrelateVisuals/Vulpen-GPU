#pragma once

#include "baseclasses/Log.h"

namespace VP {

// The only place OS APIs and OS #ifdefs appear, so everything above stays portable.
class Files {};
class Window {};
class Terminal {};

} // namespace VP
