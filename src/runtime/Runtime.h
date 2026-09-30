#pragma once

#include <span>
#include <string_view>

namespace VP {

// The top of the tree: builds every module and runs the loop, so main() only calls this
// and a test can call it without a window. The runtime lives in Runtime.cpp, so nothing
// that calls it compiles the modules it owns.
//
// build: what this binary is, which the log names first (RC07). Throws when vulpen cannot
// run with the arguments; the exit code is nonzero when the view ends the run with a
// node in error.
int run(std::span<char *const> arguments, std::string_view build);

} // namespace VP
