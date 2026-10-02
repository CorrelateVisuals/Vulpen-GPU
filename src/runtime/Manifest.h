#pragma once

#include <filesystem>

namespace VP {

struct View;

// The .vlp text of a view, both ways. Load, save and migrate are its own commands, so
// they land in the command log like any edit.
class Manifest {
public:
  // Throws naming the file and line of the first mistake (A02).
  static View load(const std::filesystem::path &file);
  // Writes the view over its file in the words load reads, through Files::save (RA04).
  // A comment a person wrote stays over the line it was written over, or at the end of
  // the line it ended, while that line's section or word lives on.
  static void save(const View &view);
};

} // namespace VP
