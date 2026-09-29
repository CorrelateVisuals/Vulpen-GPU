#pragma once

#include "runtime/View.h"

#include <filesystem>

namespace VP {

// The .vlp text of a view, both ways. Load, save and migrate are its own commands, so
// they land in the command log like any edit.
class Manifest {
public:
  // Throws naming the file and line of the first mistake (A02).
  static View load(const std::filesystem::path &file);
};

} // namespace VP
