#pragma once

#include "runtime/View.h"

#include <array>
#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace VP_VIEW {

// What links two files, by the layer the graph draws it in, a hue of the theme each,
// which its relations param turns on and off: flow, a connection, which the graph reads
// from the view itself; structure, a recipe a node uses; code, a file including or
// importing another; build, a CMake file naming a file or a folder; docs, a document
// linking a file, or a file citing an id a document defines.
enum class Layer : std::uint8_t { flow, structure, code, build, docs };
inline constexpr auto layer_names =
    std::to_array<std::string_view>({"flow", "structure", "code", "build", "docs"});

// From the file that says it to the file or folder it names, by absolute path, so a
// relation reaching past the folder shown keeps both ends.
struct Relation {
  std::string from;
  std::string to;
  Layer layer = Layer::code;

  auto operator<=>(const Relation &) const = default;
};

// What the relations part hands the graph: the relations among the files of the view
// shown, or of the folder its folder param names, which the graph then shows in place of
// the view, as a view's nodes, a node a folder, named by its path from the folder holding
// the one opened, joined by /; and the node and its view, so the graph sends the line
// that shows the view again.
struct Relations {
  std::string node;
  std::string view;
  std::string folder; // absolute; empty for the view
  VP::View tree;      // the folder's
  std::vector<Relation> relations;
  std::uint64_t changes = 0; // the readings that changed what it holds, so far
};

} // namespace VP_VIEW
