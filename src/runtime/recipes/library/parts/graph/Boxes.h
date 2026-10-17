#pragma once

#include "contracts/Palette.h"
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "Arranged.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Only Graph.cpp includes this header, so what it holds stays in the unnamed namespace of
// the one file that builds the part, as the class does: the boxes the graph shows, a
// level of the view at a time, what each lists, and the ends of the connections between
// them.
namespace {

constexpr std::uint32_t node_room = 256; // boxes shown at most; the rest are cut
constexpr std::size_t no_box = static_cast<std::size_t>(-1);
// What a view's box calls what it draws into its window, which a connection may take.
constexpr std::string_view window_port = "window";
constexpr std::string_view manifest_name = "view.vlp";
// The title of the boxes standing for what lies outside the node the graph shows the
// inside of, whose ports a connection reaching in or out leaves or reaches.
constexpr std::string_view outside_title = "outside";
// Where an end shows while the box of what lies outside is not made yet.
constexpr std::size_t outside_box = no_box - 1;

// A row a box lists under its name: a port, the recipe its node uses, a node inside it,
// or a file. A port row holds a port the node reads, at the left edge, and one it writes,
// at the right, so a curve lands on the port it joins.
struct BoxRow {
  std::string text;    // the port read, the recipe, the node inside or the file
  std::string written; // the port written
  std::uint32_t role = VP_VIEW::role("text");
  std::filesystem::path opens; // what a press on it opens; empty for none
};

// A node, a view the view shown hosts, or what lies outside the node the graph shows the
// inside of, and the rows it lists: its ports first, then the recipe it uses, then the
// nodes inside it, then its files. A box of a node holding nodes lists them, and shows
// them once the graph shows its inside, framed by its box.
struct Box {
  std::string name;  // as select, the .ini and a line name it
  std::string title; // its first row: its name, past the box it is in
  std::size_t parent = no_box;
  Kind kind = Kind::group;
  // A node of the view, or a view it hosts, which a press selects and a drag places; a
  // node of a recipe a node uses is not, nor the frame, nor what lies outside it.
  bool own = false;
  bool holds = false;    // it holds nodes, whose insides zooming into it or a second
                         // press on it shows
  std::size_t first = 0; // its rows, in Boxes::rows
  std::size_t rows = 0;
  std::size_t ports = 0; // of its rows, the first, a port read or written on each
};

// A curve: from the row of a writer's port to the row of a reader's, a reader each.
struct End {
  std::size_t wire = 0; // the connection, whose curves share their lanes
  std::size_t from = 0;
  std::size_t from_row = 0;
  std::size_t to = 0;
  std::size_t to_row = 0;
};

// What the graph shows of a view, built again only when the view or the level shown
// changes, so a frame allocates nothing for it: each box after the box it is inside, so
// the boxes drawn last lie over the others, then the views it hosts, or what lies
// outside.
struct Boxes {
  std::vector<Box> boxes;
  std::vector<BoxRow> rows;
  std::vector<End> ends;
  std::size_t frame = no_box; // the box of the node whose inside shows, framing the rest
};

// A box's rows, as its first and count give them.
std::span<const BoxRow> rows_in(const Boxes &boxes, std::size_t box) {
  return std::span(boxes.rows).subspan(boxes.boxes[box].first, boxes.boxes[box].rows);
}

// The box of a view the view shown hosts, by its name; past the last for none.
std::size_t view_box(const Boxes &boxes, std::string_view name) {
  return static_cast<std::size_t>(
      std::ranges::find_if(boxes.boxes,
                           [&](const Box &box) {
                             return box.kind == Kind::view && box.name == name;
                           }) -
      boxes.boxes.begin());
}

// The ports of a box, in the order the connections name them: those it reads and those
// it writes, each once.
struct Ports {
  std::vector<std::string> read;
  std::vector<std::string> written;
};

// A port named once, however many connections name it; where it is among the others.
std::size_t port_row(std::vector<std::string> &ports, std::string_view port) {
  const auto found = std::ranges::find(ports, port);
  if (found != ports.end())
    return static_cast<std::size_t>(found - ports.begin());
  ports.emplace_back(port);
  return ports.size() - 1;
}

// The node its parent's name names, by the longest name of a node it is inside: the
// names of a view's nodes join their folders by dots (RV08), and those of a folder's by /.
std::size_t parent_of(const std::vector<VP::Node> &nodes,
                      std::string_view name,
                      char joins = '.') {
  for (std::size_t dot = name.rfind(joins); dot != std::string_view::npos;
       dot = name.rfind(joins, dot - 1)) {
    const auto found = std::ranges::find(nodes, name.substr(0, dot), &VP::Node::name);
    if (found != nodes.end())
      return static_cast<std::size_t>(found - nodes.begin());
    if (dot == 0)
      break;
  }
  return no_box;
}

// What the graph knows of the view's nodes while it builds the boxes: whether they are a
// folder's, each node's parent, the box showing it, and what lies outside the node whose
// inside shows, at the left, where connections reach in from, and at the right, where
// they reach out to.
struct Building {
  bool folders = false;
  std::vector<std::size_t> parents;
  std::vector<std::size_t> box_of_node;
  std::array<std::size_t, 2> outside{no_box, no_box};
  std::vector<Ports> ports;
};

// The box showing a node: its own, or that of the node holding it that a box shows; none
// for a node outside the node whose inside shows.
std::size_t holder_of(const Building &building, std::size_t node) {
  for (std::size_t at = node; at != no_box; at = building.parents[at])
    if (building.box_of_node[at] != no_box)
      return building.box_of_node[at];
  return no_box;
}

// The box of what lies outside, at the left or the right, made as a connection first
// needs it.
std::size_t outside_of(Boxes &boxes, Building &building, bool left) {
  std::size_t &outside = building.outside[left ? 0 : 1];
  if (outside == no_box && boxes.boxes.size() < node_room) {
    outside = boxes.boxes.size();
    boxes.boxes.push_back({.title = std::string(outside_title)});
    building.ports.emplace_back();
  }
  return outside;
}

// Where an end of a connection shows, a box and the port's name there: a view's box, the
// box showing its node, named from that box, as split.area on a dock's box, or outside,
// named whole. No box, for a node no box shows and none is outside, as at the top.
std::pair<std::size_t, std::string> shown(const Boxes &boxes,
                                          const Building &building,
                                          const VP::View &flat,
                                          const VP::Endpoint &end) {
  std::size_t box = no_box;
  if (end.view()) {
    box = view_box(boxes, end.node);
  } else if (const auto node = std::ranges::find(flat.nodes, end.node, &VP::Node::name);
             node != flat.nodes.end()) {
    box = holder_of(building, static_cast<std::size_t>(node - flat.nodes.begin()));
  } else {
    return {no_box, {}};
  }
  if (box < boxes.boxes.size()) {
    const std::string &holder = boxes.boxes[box].name;
    if (end.view() || end.node == holder)
      return {box, end.view() ? std::string(window_port) : end.port};
    return {box, std::format("{}.{}", end.node.substr(holder.size() + 1), end.port)};
  }
  if (boxes.frame == no_box)
    return {no_box, {}};
  return {outside_box,
          end.view() ? std::format("{}.{}", end.node, window_port) : end.text()};
}

// The ports each box reads and writes, and a curve for each reader of each connection
// between two boxes, from where its writer shows to where the reader does. A connection
// inside one box, or wholly outside, draws nothing at this level.
void connect(Boxes &boxes, Building &building, const VP::View &flat) {
  for (std::size_t wire = 0; wire < flat.connections.size(); ++wire) {
    const VP::Connection &connection = flat.connections[wire];
    const auto [writer, written] = shown(boxes, building, flat, connection.from);
    for (const VP::Endpoint &to : connection.to) {
      const auto [shown_reader, read] = shown(boxes, building, flat, to);
      if (writer == no_box || shown_reader == no_box || writer == shown_reader)
        continue;
      const std::size_t from =
          writer == outside_box ? outside_of(boxes, building, true) : writer;
      const std::size_t reader =
          shown_reader == outside_box ? outside_of(boxes, building, false) : shown_reader;
      if (from == no_box || reader == no_box)
        continue;
      boxes.ends.push_back({.wire = wire,
                            .from = from,
                            .from_row = port_row(building.ports[from].written, written),
                            .to = reader,
                            .to_row = port_row(building.ports[reader].read, read)});
    }
  }
}

// Each box's rows: its ports, a reader's and a writer's side by side, then the recipe its
// node uses, whose row opens the recipe's manifest, then the nodes inside it, unless it
// frames them, then its files, each opening itself.
void list_rows(Boxes &boxes,
               std::size_t box,
               const Building &building,
               const VP::View &flat,
               const VP::Child *child) {
  Box &listed = boxes.boxes[box];
  const Ports &ports = building.ports[box];
  listed.first = boxes.rows.size();
  listed.ports = std::max(ports.read.size(), ports.written.size());
  for (std::size_t at = 0; at < listed.ports; ++at)
    boxes.rows.push_back(
        {.text = at < ports.read.size() ? ports.read[at] : std::string(),
         .written = at < ports.written.size() ? ports.written[at] : std::string(),
         .role = VP_VIEW::role("muted")});
  const auto node = std::ranges::find(building.box_of_node, box);
  if (node != building.box_of_node.end()) {
    const auto at = static_cast<std::size_t>(node - building.box_of_node.begin());
    const VP::Node &node_of = flat.nodes[at];
    if (node_of.uses())
      boxes.rows.push_back({.text = std::format("recipe {}", node_of.recipe),
                            .role = VP_VIEW::role("muted"),
                            .opens = node_of.folder / manifest_name});
    for (std::size_t inner = 0; inner < flat.nodes.size() && box != boxes.frame; ++inner)
      if (building.parents[inner] == at) {
        const std::string held = flat.nodes[inner].name.substr(node_of.name.size() + 1);
        boxes.rows.push_back(
            {.text = building.folders ? held + '/' : std::format("node {}", held),
             .role = VP_VIEW::role("muted")});
      }
    for (const std::string &file : node_of.files)
      boxes.rows.push_back({.text = file, .opens = node_of.folder / file});
  }
  if (child)
    boxes.rows.push_back(
        {.text = child->file.filename().generic_string(), .opens = child->file});
  listed.rows = boxes.rows.size() - listed.first;
}

// The node whose inside shows: the scope named, where it holds nodes; else none, and the
// top shows.
std::size_t scope_of(const VP::View &flat,
                     const std::vector<std::size_t> &parents,
                     std::string_view scope) {
  const auto found = std::ranges::find(flat.nodes, scope, &VP::Node::name);
  const auto at = static_cast<std::size_t>(found - flat.nodes.begin());
  return found != flat.nodes.end() && std::ranges::find(parents, at) != parents.end()
             ? at
             : no_box;
}

// The boxes of a view as it runs, each recipe a node uses unfolded, or of a folder, a
// level at a time: with a scope that holds nodes, its box framing the boxes of the nodes
// inside it, and beside it what lies outside it, as connections reach in at its left and
// out at its right; with none, the boxes of the nodes at the top and of the views the
// view hosts, as the view as edited names them. A node of the view as edited is its own;
// one a recipe brings is not, nor the frame, nor a folder, which no line selects.
Boxes boxes_of(const VP::View &edited,
               const VP::View &flat,
               std::string_view scope,
               bool folders) {
  Boxes boxes;
  Building building{.folders = folders};
  for (const VP::Node &node : flat.nodes)
    building.parents.push_back(parent_of(flat.nodes, node.name, folders ? '/' : '.'));
  building.box_of_node.assign(flat.nodes.size(), no_box);
  const std::size_t inside = scope_of(flat, building.parents, scope);
  const auto add = [&](std::size_t at, std::size_t parent) {
    if (boxes.boxes.size() >= node_room)
      return;
    const VP::Node &node = flat.nodes[at];
    building.box_of_node[at] = boxes.boxes.size();
    const std::size_t past = parent == no_box ? 0 : boxes.boxes[parent].name.size() + 1;
    boxes.boxes.push_back(
        {.name = node.name,
         .title = node.name.substr(past),
         .parent = parent,
         .kind = kind_of(node),
         .own = !folders && (inside == no_box || parent != no_box) &&
                std::ranges::find(edited.nodes, node.name, &VP::Node::name) !=
                    edited.nodes.end(),
         .holds = std::ranges::find(building.parents, at) != building.parents.end()});
  };
  if (inside != no_box) {
    boxes.frame = 0;
    add(inside, no_box);
  }
  for (std::size_t at = 0; at < flat.nodes.size(); ++at)
    if (building.parents[at] == inside)
      add(at, inside == no_box ? no_box : boxes.frame);
  for (const VP::Child &child : edited.children)
    if (inside == no_box && boxes.boxes.size() < node_room)
      boxes.boxes.push_back(
          {.name = child.name, .title = child.name, .kind = Kind::view, .own = true});
  building.ports.resize(boxes.boxes.size());
  connect(boxes, building, flat);
  for (std::size_t box = 0; box < boxes.boxes.size(); ++box) {
    const auto child = std::ranges::find(edited.children, boxes.boxes[box].name,
                                         &VP::Child::name);
    list_rows(boxes,
              box,
              building,
              flat,
              boxes.boxes[box].kind == Kind::view && child != edited.children.end()
                  ? &*child
                  : nullptr);
  }
  return boxes;
}

// A folder as a view's nodes, so the graph shows it as it shows a view: a node a folder,
// named by its path from the folder holding the one opened, joined by /, holding its
// files as the file port lists them, the hidden ones, as .git, left out as a manifest
// leaves them. Only the folder whose inside shows, inside if it names one in the folder
// opened, else that one, the folders in it with their files, and theirs by name, so a
// box lists what it holds; deeper ones are read once shown.
VP::View folder_view(VP::FilePort &files,
                     const std::filesystem::path &opened,
                     std::string_view inside) {
  const std::filesystem::path base = opened.parent_path();
  const std::filesystem::path named = base / std::string(inside);
  const std::filesystem::path within = named.lexically_relative(opened);
  const bool in = !inside.empty() && !within.empty() && *within.begin() != ".." &&
                  !files.list(named.string()).empty();
  const std::filesystem::path shown = in ? named : opened;
  VP::View view;
  const auto add = [&](const std::filesystem::path &folder, bool read) {
    view.nodes.push_back(
        {.name = folder.lexically_relative(base).generic_string(), .folder = folder});
    std::vector<std::string> held;
    if (!read)
      return held;
    for (const std::string &name : files.list(folder.string())) {
      if (name.starts_with('.'))
        continue;
      if (name.ends_with('/'))
        held.push_back(name.substr(0, name.size() - 1));
      else
        view.nodes.back().files.push_back(name);
    }
    return held;
  };
  for (const std::string &inner : add(shown, true))
    for (const std::string &deeper : add(shown / inner, true))
      add(shown / inner / deeper, false);
  return view;
}

} // namespace
