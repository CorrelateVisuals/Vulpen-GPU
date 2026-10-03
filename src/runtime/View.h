#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace VP {

struct Param {
  std::string key;
  std::string value;
};

// One manifest section. A node is the folder its name names (RV08): ui.panel is ui/panel/
// in its view, beside the files of ui itself in ui/.
struct Node {
  std::string name; // the names of the nodes it is inside, then its own, joined by dots
  // In a view, the library recipe a drop copied it from and that copy's fingerprint, as
  // probe@3f2a9c1e. In the library, a recipe it uses as it is (V11), whose node it
  // becomes.
  std::string recipe;
  std::string operator_name;
  // The files in its folder, sorted: the folder decides, so a file put there is one of
  // them. Its .vert and .frag make it a draw and its .comp a dispatch, as glslang reads
  // the extension.
  std::vector<std::string> files;
  std::uint32_t invocations = 0;  // a dispatch's threads
  std::uint32_t vertex_count = 0; // a draw's vertices per instance
  std::uint32_t instance_count =
      0; // a draw's instances; 0 when not given, which runs one
  std::vector<Param> params;
  std::string log;
  // The file and line that last added or changed it, which its errors name; empty after
  // a line typed or sent. A save leaves it out.
  std::string where;
  // Where its files are, which a path its C++ names starts from (RP02), and that folder
  // from its view's root, or the library's, as the build tree mirrors it. Unfolding the
  // view fills both, and a save leaves them out.
  std::filesystem::path folder;
  std::filesystem::path module;

  // Whether it uses a recipe of the library as it is, as only the library's own nodes
  // do; a drop's copy names its recipe with an @ and its fingerprint.
  bool uses() const {
    return !recipe.empty() && recipe.find('@') == std::string::npos;
  }
};

struct Endpoint {
  std::string node;
  std::string port;
};

// One buffer, shared by the node that writes it and the nodes that read it.
struct Connection {
  std::string name;
  Endpoint from;
  std::vector<Endpoint> to;
};

// A view this one hosts (V03), which runs with its own schedule and is addressed by its
// name: <name>/view.vlp beside this view's manifest, unless the manifest names a file.
struct Child {
  std::string name;
  std::filesystem::path file; // absolute
  std::string where;          // as a node's
};

// A view is a project: the views it hosts, its nodes and their connections. The model
// includes nothing, so it can hold no GPU or OS type.
struct View {
  std::string name; // its folder's name, which the build tree mirrors
  std::filesystem::path file;
  std::vector<Child> children;
  std::vector<Node> nodes;
  std::vector<Connection> connections;
};

} // namespace VP
