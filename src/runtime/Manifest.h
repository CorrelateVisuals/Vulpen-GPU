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
  // A view with nothing in it yet, named as load names it, which a save writes.
  static View empty(const std::filesystem::path &file);
  // Writes the view over its file in the words load reads, through Files::save (RA04).
  // A comment a person wrote stays over the line it was written over, or at the end of
  // the line it ended, while that line's section or word lives on.
  static void save(const View &view);
  // The view as the schedule runs it: each deploy's nodes and connections, from its
  // recipe's view.vlp in the view's recipes, named <deploy>.<name> and with the params
  // the deploy sets. A recipe may deploy others, never itself (RV06). Throws naming the
  // deploy, and what it lacks.
  static View flatten(const View &view);
  // The folder a view's files are in, which the live scan watches: the view's own, or
  // the whole library for a recipe of the library run as a view.
  static std::filesystem::path root(const View &view);
};

} // namespace VP
