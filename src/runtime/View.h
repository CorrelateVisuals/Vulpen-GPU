#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace VP {

// The rules of the graph, written once here, where the engine and every part that reads
// the graph look (C02).

// A view's manifest in its folder, and the folders at its top that hold the contracts and
// the code its nodes share (RV05, RV09), which are no node's.
inline constexpr std::string_view manifest_file = "view.vlp";
inline constexpr std::string_view contracts_folder = "contracts";
inline constexpr std::string_view shared_folder = "shared";

// A hidden file, or one in a hidden folder, as .vulpen/ in a view's, which parts rewrite
// as they go and no node's files list; the folders above it may be hidden too, as a
// person's checkout can be.
inline bool hidden(const std::filesystem::path &path) {
  const std::filesystem::path normal = path.lexically_normal();
  const auto dotted = [](const std::filesystem::path &part) {
    return part.string().starts_with('.');
  };
  return dotted(normal.filename()) || dotted(normal.parent_path().filename());
}

// A name joins a manifest's section headers, its endpoints, the command line and a
// folder's name, so it holds only what each of them leaves whole.
inline bool is_name(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, [](char letter) {
    return (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
           (letter >= '0' && letter <= '9') || letter == '_' || letter == '-';
  });
}

// Names joined by dots: the nodes a node is inside, then its own, as its folder is the
// path of their folders (RV08).
inline bool is_path(std::string_view text) {
  for (std::size_t dot = text.find('.'); dot != std::string_view::npos;
       dot = text.find('.')) {
    if (!is_name(text.substr(0, dot)))
      return false;
    text.remove_prefix(dot + 1);
  }
  return is_name(text);
}

// The words of a node section (V04), in the order a save writes them: where a node came
// from and what it runs, then its settings. Its files are what its folder holds (RV08),
// so no word lists them.
inline constexpr std::array<std::string_view, 8> node_words{"recipe",
                                                            "operator",
                                                            "invocations",
                                                            "vertex_count",
                                                            "instance_count",
                                                            "param",
                                                            "image",
                                                            "log"};

// A node's folder in its view: ui.panel is ui/panel (RV08).
inline std::filesystem::path path_of(std::string_view node) {
  std::filesystem::path path;
  for (std::size_t dot = node.find('.'); dot != std::string_view::npos;
       dot = node.find('.')) {
    path /= node.substr(0, dot);
    node.remove_prefix(dot + 1);
  }
  return path / node;
}

// The view the recipes build as, the library's, and each app's beside it, whose own node
// is named like its folder already, as nodes.cmake names it.
inline constexpr std::string_view library_view = "library";

// A project's top folder is a node too, named ., the folder itself as a path (RV08): the
// files beside its manifest are its files, and [node "."] holds its words, so no word is
// new (V04). The nodes in it are named from the top, as ever, and a port of it is .port.
// The library's recipes have none: each is named like its folder already.
inline constexpr std::string_view top_node = ".";

// Whether the node named is the other or inside it, by their names.
inline bool within(std::string_view node, std::string_view other) {
  return node.starts_with(other) &&
         (node.size() == other.size() || node[other.size()] == '.');
}

struct Param {
  std::string key;
  std::string value;
};

// The format of an image its C++ fills, as Vulkan names it (VK04): an image word.
struct ImagePort {
  std::string port;
  std::string format;
};

// One manifest section. A node is the folder its name names (RV08): ui.panel is ui/panel/
// in its view, beside the files of ui itself in ui/.
struct Node {
  std::string name; // the names of the nodes it is inside, then its own, joined by dots
  // In a view, the library recipe a drop copied it from and that copy's fingerprint, as
  // probe@3f2a9c1e. In the library, a recipe it uses as it is (V11), whose node it
  // becomes, and which that node still names once unfolded.
  std::string recipe;
  std::string operator_name;
  // The files in its folder, sorted: the folder decides, so a file put there is one of
  // them. Its .vert and .frag make it a draw, with a .geom and a .tesc and .tese between
  // them if it holds them, and its .comp a dispatch, as glslang reads the extension.
  std::vector<std::string> files;
  std::uint32_t invocations = 0;  // a dispatch's threads
  // In place of a number: the port of a buffer it reads, whose length counts them, so a
  // count shared along a chain is written once.
  std::string invocations_port;
  std::uint32_t vertex_count = 0; // a draw's vertices per instance
  // A draw's instances: a number, or the port of the buffer whose used length counts
  // them each frame; empty when not given, which runs one.
  std::string instance_count;
  std::vector<Param> params;
  std::vector<ImagePort> images; // one it does not name takes R8G8B8A8_UNORM
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
  // do, or, unfolded, became one; a drop's copy names its recipe with an @ and its
  // fingerprint.
  bool uses() const {
    return !recipe.empty() && recipe.find('@') == std::string::npos;
  }
  bool counts_invocations() const {
    return invocations != 0 || !invocations_port.empty();
  }
  // The param it sets of that key; null for none.
  const Param *param(std::string_view key) const {
    const auto found = std::ranges::find(params, key, &Param::key);
    return found == params.end() ? nullptr : &*found;
  }
};

// The build compiles each shader as the stage its extension names, as glslang does, so
// the loader reads the stage from the same place.
inline constexpr std::string_view compute_stage = ".comp";
inline constexpr std::string_view vertex_stage = ".vert";
inline constexpr std::string_view tessellation_control_stage = ".tesc";
inline constexpr std::string_view tessellation_evaluation_stage = ".tese";
inline constexpr std::string_view geometry_stage = ".geom";
inline constexpr std::string_view fragment_stage = ".frag";
inline constexpr std::array<std::string_view, 6> stages{compute_stage,
                                                        vertex_stage,
                                                        tessellation_control_stage,
                                                        tessellation_evaluation_stage,
                                                        geometry_stage,
                                                        fragment_stage};

inline bool holds_stage(const Node &node, std::string_view stage) {
  return std::ranges::any_of(
      node.files, [&](const std::string &file) { return file.ends_with(stage); });
}

// A node's port, or with no port a view this view hosts, as `<view>:`: what the view
// draws into its window, as one image, which only the view that hosts it takes (V03).
struct Endpoint {
  std::string node;
  std::string port;

  bool view() const {
    return port.empty();
  }
  // As a manifest and a command write it: a port of the top node as .port.
  std::string text() const {
    return view() ? node + ':' : (node == top_node ? "" : node) + '.' + port;
  }
};

// One buffer, shared by the node that writes it and the nodes that read it.
struct Connection {
  std::string name;
  Endpoint from;
  std::vector<Endpoint> to;
};

// One word of a section and its value, as a save writes it.
struct Word {
  std::string_view key;
  std::string value;
};

// A node's words in the order of node_words, a param or an image a word each, as a save
// writes them and info shows them.
inline std::vector<Word> words_of(const Node &node) {
  std::vector<Word> words;
  if (!node.recipe.empty())
    words.push_back({"recipe", node.recipe});
  if (!node.operator_name.empty())
    words.push_back({"operator", node.operator_name});
  if (node.invocations != 0)
    words.push_back({"invocations", std::to_string(node.invocations)});
  else if (!node.invocations_port.empty())
    words.push_back({"invocations", node.invocations_port});
  if (node.vertex_count != 0)
    words.push_back({"vertex_count", std::to_string(node.vertex_count)});
  if (!node.instance_count.empty())
    words.push_back({"instance_count", node.instance_count});
  for (const Param &param : node.params)
    words.push_back({"param", param.key + '=' + param.value});
  for (const ImagePort &image : node.images)
    words.push_back({"image", image.port + '=' + image.format});
  if (!node.log.empty())
    words.push_back({"log", node.log});
  return words;
}

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

// The node of a port as a line names it, node.port: the top's for .port, and the whole
// for a name with no dot.
inline std::string_view port_node(std::string_view port) {
  const std::size_t dot = port.rfind('.');
  if (dot == std::string_view::npos)
    return port;
  return dot == 0 ? top_node : port.substr(0, dot);
}

// Whether a node is a project's top folder's, top_node.
inline bool is_top(const View &view, std::string_view node) {
  return node == top_node && view.name != library_view;
}

// A node's folder from its view's folder: its name as a path, or none for the top.
inline std::filesystem::path folder_in(const View &view, std::string_view node) {
  return is_top(view, node) ? std::filesystem::path() : path_of(node);
}

// Whether a draw of the view renders into its window, so the view needs one, as the
// engine opens a window by it and a GUI tells a project with something to show: a draw no
// connection starts from, by the node holding its .frag, a fragment node's for a draw
// split over two. One whose output is connected renders into an image instead.
inline bool draws_into_window(const View &view) {
  return std::ranges::any_of(view.nodes, [&](const Node &node) {
    return holds_stage(node, fragment_stage) &&
           std::ranges::none_of(view.connections, [&](const Connection &connection) {
             return connection.from.node == node.name;
           });
  });
}

} // namespace VP
