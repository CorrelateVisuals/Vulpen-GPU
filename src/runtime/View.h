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

// One manifest entry: the recipe it comes from, the C++ class and shader it runs, and
// the params they read.
struct Node {
  std::string name;
  std::string recipe;
  std::string operator_name;
  std::string shader;
  std::uint32_t invocations = 0;
  std::vector<Param> params;
  std::string log;
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

// A view is a project: its nodes, their connections, and the child views it hosts.
// The model includes nothing, so it can hold no GPU or OS type.
struct View {
  std::string name; // its folder's name, which the build tree mirrors
  std::filesystem::path file;
  std::vector<Node> nodes;
  std::vector<Connection> connections;
};

} // namespace VP
