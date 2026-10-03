#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace VP {

struct View;

// The .vlp text of a view, both ways, and the folders it names. Load, save and migrate
// are its own commands, so they land in the command log like any edit.
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
  // Each node's files as its folder holds them now (RV08), so a file put there is one of
  // them. Returns what changed since the view last listed them, for the log.
  static std::vector<std::string> refresh(View &view);
  // Each folder in the view that no node names, for the log: every folder is a node.
  static std::vector<std::string> unnamed(const View &view);
  // The view as the schedule runs it: each node with its folder, and in a recipe of the
  // library each recipe a node uses unfolded into that node and the nodes inside it,
  // with the params the node sets (V11). Throws naming what does not unfold: a recipe
  // the library lacks, uses that form a cycle (RV06), a node inside no node.
  static View flatten(const View &view);
  // The folder a view's files are in, which the live scan watches: the view's own, or
  // the whole library for a recipe of the library run as a view.
  static std::filesystem::path root(const View &view);
};

} // namespace VP
