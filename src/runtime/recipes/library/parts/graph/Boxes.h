#pragma once

#include "contracts/Palette.h"
#include "runtime/View.h"
#include "Arranged.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Only Graph.cpp includes this header, so what it holds stays in the unnamed namespace of
// the one file that builds the part, as the class does: the boxes the graph shows, nested
// as their nodes are, what each lists, and the ends of the connections between them.
namespace {

constexpr std::uint32_t node_room = 256; // boxes shown at most; the rest are cut
constexpr std::size_t no_box = static_cast<std::size_t>(-1);
// What a view's box calls what it draws into its window, which a connection may take.
constexpr std::string_view window_port = "window";
constexpr std::string_view manifest_name = "view.vlp";

// A row a box lists under its name: a port, the recipe its node uses, or a file. A port
// row holds a port the node reads, at the left edge, and one it writes, at the right, so
// a curve lands on the port it joins.
struct BoxRow {
  std::string text;    // the port read, the recipe or the file
  std::string written; // the port written
  std::uint32_t role = VP_VIEW::role("text");
  std::filesystem::path opens; // what a press on it opens; empty for none
};

// A node, or a view the view shown hosts, and the rows it lists: its ports first, then
// the recipe it uses, then its files. A node inside another shows inside its box, as its
// folder is inside that node's (RV08), so a recipe a node uses shows whole in its box.
struct Box {
  std::string name;  // as select, the .ini and a line name it
  std::string title; // its first row: its name, past the box it is in
  std::size_t parent = no_box;
  Kind kind = Kind::group;
  // A node of the view, or a view it hosts, which a press selects and a drag places; a
  // node of a recipe a node uses is part of that node's box.
  bool own = false;
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

// What the graph shows of a view, built again only when the view changes, so a frame
// allocates nothing for it: each box after the box it is inside, so the boxes drawn last
// lie over the others, then the views it hosts.
struct Boxes {
  std::vector<Box> boxes;
  std::vector<BoxRow> rows;
  std::vector<End> ends;
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

// The box its parent's name names, by the longest name of a node it is inside.
std::size_t parent_of(const std::vector<VP::Node> &nodes, std::string_view name) {
  for (std::size_t dot = name.rfind('.'); dot != std::string_view::npos;
       dot = name.rfind('.', dot - 1)) {
    const auto found = std::ranges::find(nodes, name.substr(0, dot), &VP::Node::name);
    if (found != nodes.end())
      return static_cast<std::size_t>(found - nodes.begin());
    if (dot == 0)
      break;
  }
  return no_box;
}

// The boxes in the order drawn: a box, then the boxes inside it, each in the manifest's
// order, as many as there is room for.
void order_boxes(const std::vector<VP::Node> &nodes,
                 const std::vector<std::size_t> &parents,
                 std::size_t parent,
                 std::vector<std::size_t> &order) {
  for (std::size_t at = 0; at < nodes.size() && order.size() < node_room; ++at)
    if (parents[at] == parent) {
      order.push_back(at);
      order_boxes(nodes, parents, at, order);
    }
}

// Each box's rows: its ports, a reader's and a writer's side by side, then the recipe its
// node uses, whose row opens the recipe's manifest, then its files, each opening itself.
void list_rows(Boxes &boxes,
               std::size_t box,
               const Ports &ports,
               const VP::Node *node,
               const VP::Child *child) {
  Box &shown = boxes.boxes[box];
  shown.first = boxes.rows.size();
  shown.ports = std::max(ports.read.size(), ports.written.size());
  for (std::size_t at = 0; at < shown.ports; ++at)
    boxes.rows.push_back(
        {.text = at < ports.read.size() ? ports.read[at] : std::string(),
         .written = at < ports.written.size() ? ports.written[at] : std::string(),
         .role = VP_VIEW::role("muted")});
  if (node && node->uses())
    boxes.rows.push_back({.text = std::format("recipe {}", node->recipe),
                          .role = VP_VIEW::role("muted"),
                          .opens = node->folder / manifest_name});
  if (node)
    for (const std::string &file : node->files)
      boxes.rows.push_back({.text = file, .opens = node->folder / file});
  if (child)
    boxes.rows.push_back(
        {.text = child->file.filename().generic_string(), .opens = child->file});
  shown.rows = boxes.rows.size() - shown.first;
}

// The ports each box reads and writes, and a curve for each reader of each connection,
// from the box of the node writing it, or of the view whose window it takes. A connection
// to a node no box shows draws none.
void connect(Boxes &boxes,
             const VP::View &flat,
             const std::vector<std::size_t> &box_of_node,
             std::vector<Ports> &ports) {
  const auto box_of = [&](const VP::Endpoint &end) {
    if (end.view())
      return view_box(boxes, end.node);
    const auto node = std::ranges::find(flat.nodes, end.node, &VP::Node::name);
    return node == flat.nodes.end()
               ? boxes.boxes.size()
               : box_of_node[static_cast<std::size_t>(node - flat.nodes.begin())];
  };
  for (std::size_t wire = 0; wire < flat.connections.size(); ++wire) {
    const VP::Connection &connection = flat.connections[wire];
    const std::size_t from = box_of(connection.from);
    if (from >= boxes.boxes.size())
      continue;
    const std::size_t from_row = port_row(
        ports[from].written, connection.from.view() ? window_port : connection.from.port);
    for (const VP::Endpoint &to : connection.to)
      if (const std::size_t reader = box_of(to); reader < boxes.boxes.size())
        boxes.ends.push_back({.wire = wire,
                              .from = from,
                              .from_row = from_row,
                              .to = reader,
                              .to_row = port_row(ports[reader].read, to.port)});
  }
}

// The boxes of a view as it runs, each recipe a node uses unfolded, so each node of a
// recipe shows in its user's box; and the views it hosts, as the view as edited names
// them. A node of the view as edited is its own; one a recipe brings is not.
Boxes boxes_of(const VP::View &edited, const VP::View &flat) {
  Boxes boxes;
  std::vector<std::size_t> parents;
  for (const VP::Node &node : flat.nodes)
    parents.push_back(parent_of(flat.nodes, node.name));
  std::vector<std::size_t> order;
  order_boxes(flat.nodes, parents, no_box, order);
  std::vector<std::size_t> box_of_node(flat.nodes.size(), no_box);
  for (const std::size_t at : order) {
    const VP::Node &node = flat.nodes[at];
    const std::size_t parent = parents[at] == no_box ? no_box : box_of_node[parents[at]];
    box_of_node[at] = boxes.boxes.size();
    boxes.boxes.push_back(
        {.name = node.name,
         .title = parent == no_box
                      ? node.name
                      : node.name.substr(boxes.boxes[parent].name.size() + 1),
         .parent = parent,
         .kind = kind_of(node),
         .own = std::ranges::find(edited.nodes, node.name, &VP::Node::name) !=
                edited.nodes.end()});
  }
  for (const VP::Child &child : edited.children)
    if (boxes.boxes.size() < node_room)
      boxes.boxes.push_back(
          {.name = child.name, .title = child.name, .kind = Kind::view, .own = true});
  std::vector<Ports> ports(boxes.boxes.size());
  connect(boxes, flat, box_of_node, ports);
  for (std::size_t box = 0; box < boxes.boxes.size(); ++box) {
    const std::size_t node = std::ranges::find(box_of_node, box) - box_of_node.begin();
    const auto child = std::ranges::find(edited.children, boxes.boxes[box].name,
                                         &VP::Child::name);
    list_rows(boxes,
              box,
              ports[box],
              node < flat.nodes.size() ? &flat.nodes[node] : nullptr,
              node < flat.nodes.size() || child == edited.children.end() ? nullptr
                                                                        : &*child);
  }
  return boxes;
}

} // namespace
