#pragma once

namespace VP {

// An image a draw renders into instead of the window, at the window's size, for another
// node to sample. Which draw reaches the screen stays a manifest fact: a draw whose
// output is unconnected renders into the window.
class Offscreen {};

} // namespace VP
