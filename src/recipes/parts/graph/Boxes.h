#pragma once

#include "runtime/View.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

// Only Graph.cpp includes this header, so what it holds stays in the unnamed namespace of
// the one file that builds the part, as the class does: which box of the graph a node, a
// view the view hosts and a connection's end are, and where a node's files are.
namespace {

constexpr std::uint32_t node_room = 256; // boxes shown at most; the rest are cut

// The node named, or the one a recipe it is inside makes it part of; none, past the
// last, for a name no node holds.
std::size_t node_of(const std::vector<VP::Node> &nodes, std::string_view name) {
  for (;;) {
    const auto found = std::ranges::find(nodes, name, &VP::Node::name);
    if (found != nodes.end())
      return static_cast<std::size_t>(found - nodes.begin());
    const std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos)
      return nodes.size();
    name = name.substr(0, dot);
  }
}

// The boxes the graph shows of a view: its nodes, then the views it hosts, as many as
// there is room for.
std::size_t boxes_of(const VP::View &view) {
  return std::min(view.nodes.size() + view.children.size(), std::size_t{node_room});
}

// The box a connection leaves: its writer's, or that of the view whose window it takes;
// none, past the last, for one the view does not show.
std::size_t writer_of(const VP::View &view, const VP::Endpoint &from) {
  if (!from.view())
    return node_of(view.nodes, from.node);
  const auto child = std::ranges::find(view.children, from.node, &VP::Child::name);
  return view.nodes.size() + static_cast<std::size_t>(child - view.children.begin());
}

// A node's folder in its view's (RV08): ui.panel is ui/panel/.
std::filesystem::path folder_of(const VP::View &view, std::string_view name) {
  std::filesystem::path folder = view.file.parent_path();
  for (std::size_t dot = name.find('.'); dot != std::string_view::npos;
       dot = name.find('.')) {
    folder /= name.substr(0, dot);
    name.remove_prefix(dot + 1);
  }
  return folder / name;
}

} // namespace
