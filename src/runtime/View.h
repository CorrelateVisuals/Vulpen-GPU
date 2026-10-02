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

// One manifest entry: the recipe it comes from, the C++ class and shaders it runs, and
// the params they read.
struct Node {
  std::string name;
  std::string recipe;
  std::string operator_name;
  std::vector<std::string> shaders; // one compute shader, or a vertex and a fragment one
  std::uint32_t invocations = 0;
  std::vector<Param> params;
  std::string log;
  // The file and line that last added or changed it, which its errors name; empty after
  // a line typed or sent. A save leaves it out.
  std::string where;
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

// A recipe the view deploys under a name, with the params it sets on the recipe's
// nodes. The loader flattens it, so the schedule sees only nodes and connections.
struct Deploy {
  std::string name;
  std::string recipe; // a folder of the view's recipes, whose view.vlp gives its nodes
  std::vector<Param> params; // each keyed <node>.<param>, by the recipe's node names
  std::string where;         // as a node's
};

// A view is a project: the recipes it deploys, its nodes, their connections, and the
// child views it hosts. The model includes nothing, so it can hold no GPU or OS type.
struct View {
  std::string name; // its folder's name, which the build tree mirrors
  std::filesystem::path file;
  std::vector<Deploy> deploys;
  std::vector<Node> nodes;
  std::vector<Connection> connections;
};

} // namespace VP
