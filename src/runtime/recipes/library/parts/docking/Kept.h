#pragma once

#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Only Docking.cpp includes this header, so what it holds stays in the unnamed namespace
// of the one file that builds the part, as the class does: what the docking part keeps
// of the panels as a person left them, between runs, in a file beside the view's
// manifest, as the graph keeps where its boxes are: the edits that make the manifest's
// panels the panels as they are, and the params of the nodes that seat them.
namespace {

constexpr std::string_view kept_folder = ".vulpen"; // which no build or scan reads
constexpr std::string_view kept_file = "layout.log";
constexpr std::string_view kept_head =
    "# The panels as a person left them, which panel restore replays as the view\n"
    "# starts as its manifest has it: the edits that make the manifest's panels these,\n"
    "# and their nodes' params. Deleting the file shows the manifest's own.\n";
// The recipes of the nodes the panels are made of, which the file keeps: the docks
// seating them, the panels, the stacks holding several contents, the editors, the decks.
constexpr std::array<std::string_view, 5> laid_recipes{"dock", "panel", "stack",
                                                       "text-area", "deck"};
constexpr std::string_view strip_params = "bar."; // a panel's strip's, which no file keeps
// The line naming what the manifest wrote as the file was kept, a comment to a replay.
constexpr std::string_view written_line = "# written ";
// FNV-1a, 64 bits: a fingerprint that reads the same on every machine and every build.
constexpr std::uint64_t fnv_offset = 14695981039346656037ull;
constexpr std::uint64_t fnv_prime = 1099511628211ull;

bool laid(const VP::Node &node) {
  return std::ranges::find(laid_recipes, node.recipe) != laid_recipes.end();
}

// A param of a panel's node the file keeps, one of a node inside it, as split.ratio; but
// its strip's, which shows at every start, so the menubar a person hid comes back.
bool kept_param(const VP::Param &param) {
  return param.key.find('.') != std::string::npos && !param.key.starts_with(strip_params);
}

const VP::Node *node_in(const VP::View &view, std::string_view name) {
  const auto found = std::ranges::find(view.nodes, name, &VP::Node::name);
  return found == view.nodes.end() ? nullptr : &*found;
}

const VP::Connection *connection_in(std::span<const VP::Connection> connections,
                                    std::string_view name) {
  const auto found = std::ranges::find(connections, name, &VP::Connection::name);
  return found == connections.end() ? nullptr : &*found;
}

bool same_ends(const VP::Connection &a, const VP::Connection &b) {
  const auto same = [](const VP::Endpoint &x, const VP::Endpoint &y) {
    return x.node == y.node && x.port == y.port;
  };
  return same(a.from, b.from) && std::ranges::equal(a.to, b.to, same);
}

// Each node to just before the node it shows before now, from the last, as node move puts
// it, where it is not already, so the nodes go in the order they draw in now; a node
// holding others stays, as the view's own group does.
void move_into_order(std::vector<std::string> &order,
                     const VP::View &now,
                     std::vector<std::string> &lines) {
  std::vector<std::string_view> wanted;
  for (const VP::Node &node : now.nodes)
    if (std::ranges::find(order, node.name) != order.end())
      wanted.push_back(node.name);
  for (std::size_t at = wanted.size(); at-- > 1;) {
    const auto moving = std::ranges::find(order, wanted[at - 1]);
    if ((moving + 1 != order.end() && *(moving + 1) == wanted[at]) ||
        std::ranges::any_of(order, [&](const std::string &name) {
          return name != *moving && VP::within(name, *moving);
        }))
      continue;
    lines.push_back(std::format("node move {} {}", wanted[at - 1], wanted[at]));
    std::string moved = std::move(*moving);
    order.erase(moving);
    order.insert(std::ranges::find(order, wanted[at]), std::move(moved));
  }
}

// Whether the unfolded manifest's node or connection of that name is one a recipe its
// nodes use brings, inside a node that uses one, which the manifest itself does not write.
bool brought(const VP::View &written, std::string_view name) {
  return std::ranges::any_of(written.nodes, [&](const VP::Node &user) {
    return user.uses() && name != user.name && VP::within(name, user.name);
  });
}

// What the manifest writes of its nodes and connections, each node's name, recipe and
// params and each connection's name and ends, as one number, which the file names, so a
// file kept for a manifest since changed is not replayed onto it, where its lines would
// land on what its names mean now, as a node renamed or made another.
std::string fingerprint_of(const VP::View &written) {
  std::uint64_t hash = fnv_offset;
  const auto add = [&](std::string_view text) {
    for (const char byte : text)
      hash = (hash ^ static_cast<unsigned char>(byte)) * fnv_prime;
    hash = (hash ^ '\n') * fnv_prime; // where one word ends, so two words never read as one
  };
  for (const VP::Node &node : written.nodes)
    if (!brought(written, node.name)) {
      add(node.name);
      add(node.recipe);
      for (const VP::Param &param : node.params) {
        add(param.key);
        add(param.value);
      }
    }
  for (const VP::Connection &connection : written.connections)
    if (!brought(written, connection.name)) {
      add(connection.name);
      add(connection.from.text());
      for (const VP::Endpoint &to : connection.to)
        add(to.text());
    }
  return std::format("{:016x}", hash);
}

// The fingerprint a kept file names; empty for none, as a file kept before files named it.
std::string_view kept_for(std::string_view text) {
  const std::size_t at = text.find(written_line);
  if (at == std::string_view::npos)
    return {};
  const std::string_view rest = text.substr(at + written_line.size());
  return rest.substr(0, rest.find('\n'));
}

// The lines that make the manifest's panels the panels now, whatever edits made them so,
// in an order a replay takes: the panels' nodes gone removed, the connections their nodes
// are part of let go of or made otherwise, their nodes added, every node moved to the
// order it draws in, those connections made, and the params of the manifest's own panels'
// nodes changed. So a panel added and gone again leaves no line, and the file is as long
// as the panels' changes, not as their history. The manifest's view comes unfolded, as
// the file port reads it, so of it only what the manifest writes counts, not what a
// recipe its nodes use brings, and a param it sets is read where it went, on the node
// inside; the view now comes as edited. A connection's ends on nodes neither the
// manifest writes nor a panel is made of, as the panes another part adds as projects
// come, which that part adds again, the file leaves out.
std::vector<std::string> edits_from(const VP::View &written, const VP::View &now) {
  const auto brought = [&](std::string_view name) { return ::brought(written, name); };
  std::vector<std::string_view> panels; // the panels' nodes, the manifest's or now's
  for (const VP::View *view : {&written, &now})
    for (const VP::Node &node : view->nodes)
      if (laid(node))
        panels.push_back(node.name);
  const auto known = [&](const VP::Endpoint &end) {
    return end.view() || node_in(written, end.node) ||
           std::ranges::any_of(panels, [&](std::string_view panel) {
             return VP::within(end.node, panel);
           });
  };
  std::vector<VP::Connection> kept_now;
  for (const VP::Connection &connection : now.connections) {
    if (!known(connection.from))
      continue;
    VP::Connection kept = connection;
    std::erase_if(kept.to, [&](const VP::Endpoint &end) { return !known(end); });
    if (!kept.to.empty())
      kept_now.push_back(std::move(kept));
  }
  const auto ours = [&](const VP::Connection *connection) {
    const auto of = [&](const VP::Endpoint &end) {
      return !end.view() && std::ranges::any_of(panels, [&](std::string_view panel) {
        return VP::within(end.node, panel);
      });
    };
    return connection && (of(connection->from) || std::ranges::any_of(connection->to, of));
  };
  std::vector<std::string> lines;
  std::vector<VP::Connection> left; // the manifest's own, as the removals leave them
  for (const VP::Connection &connection : written.connections)
    if (!brought(connection.name))
      left.push_back(connection);
  std::vector<std::string> order;
  std::vector<std::string> removed;
  for (const VP::Node &node : written.nodes) {
    if (brought(node.name))
      continue;
    if (node_in(now, node.name) || !laid(node)) {
      order.push_back(node.name);
      continue;
    }
    if (std::ranges::any_of(removed, [&](const std::string &gone) {
          return VP::within(node.name, gone);
        }))
      continue;
    removed.push_back(node.name);
    lines.push_back(std::format("node remove keep {}", node.name));
    const auto goes = [&](const VP::Endpoint &end) {
      return !end.view() && VP::within(end.node, node.name);
    };
    for (VP::Connection &connection : left)
      std::erase_if(connection.to, goes);
    std::erase_if(left, [&](const VP::Connection &connection) {
      return goes(connection.from) || connection.to.empty();
    });
  }
  for (const VP::Connection &connection : left)
    if (const VP::Connection *const now_is = connection_in(kept_now, connection.name);
        (ours(&connection) || ours(now_is)) && (!now_is || !same_ends(*now_is, connection)))
      lines.push_back(std::format("disconnect {}", connection.name));
  for (const VP::Node &node : now.nodes)
    if (laid(node) && !node_in(written, node.name)) {
      std::string line = std::format("node add {} recipe={}", node.name, node.recipe);
      for (const VP::Param &param : node.params)
        if (kept_param(param))
          line += std::format(" param={}={}", param.key, param.value);
      lines.push_back(std::move(line));
      order.push_back(node.name);
    }
  move_into_order(order, now, lines);
  for (const VP::Connection &connection : kept_now)
    if (const VP::Connection *const was = connection_in(left, connection.name);
        (ours(&connection) || ours(connection_in(written.connections, connection.name))) &&
        (!was || !same_ends(*was, connection))) {
      std::string line = std::format("connect {} {}", connection.name, connection.from.text());
      for (const VP::Endpoint &to : connection.to)
        line += ' ' + to.text();
      lines.push_back(std::move(line));
    }
  // A param of a panel's node, as split.ratio, the manifest set on the node inside it.
  for (const VP::Node &node : now.nodes) {
    if (!laid(node) || !node_in(written, node.name))
      continue;
    for (const VP::Param &param : node.params) {
      if (!kept_param(param))
        continue;
      const std::size_t dot = param.key.rfind('.');
      const std::string inside =
          std::format("{}.{}", node.name, std::string_view(param.key).substr(0, dot));
      const std::string_view key = std::string_view(param.key).substr(dot + 1);
      const VP::Node *const holding = node_in(written, inside);
      const auto was = holding ? std::ranges::find(holding->params, key, &VP::Param::key)
                               : std::vector<VP::Param>::const_iterator();
      if (!holding || was == holding->params.end() || was->value != param.value)
        lines.push_back(std::format("param set {} {} {}", inside, key, param.value));
    }
  }
  return lines;
}

// The file's text: what the manifest wrote, then the edits that make its panels the
// panels now.
std::string kept_text(const VP::View &written, const VP::View &now) {
  std::string text(kept_head);
  text.append(written_line).append(fingerprint_of(written)).append(1, '\n');
  for (const std::string &line : edits_from(written, now))
    text.append(line).append(1, '\n');
  return text;
}

// The lines a kept file holds, past its comments and its blank lines.
std::vector<std::string> kept_lines(std::string_view text) {
  std::vector<std::string> lines;
  while (!text.empty()) {
    const std::size_t end = std::min(text.find('\n'), text.size());
    const std::string_view line = text.substr(0, end);
    if (!line.empty() && !line.starts_with('#'))
      lines.emplace_back(line);
    text.remove_prefix(std::min(end + 1, text.size()));
  }
  return lines;
}

// Whether the view runs as its manifest holds it, its nodes, their params and its
// connections the same, so the kept panels go onto the view they were moved from, not
// onto one a replay of the log, as a restart's, arranged already.
bool as_written(const VP::View &running, const VP::View &written) {
  const auto same_param = [](const VP::Param &a, const VP::Param &b) {
    return a.key == b.key && a.value == b.value;
  };
  const auto same_end = [](const VP::Endpoint &a, const VP::Endpoint &b) {
    return a.node == b.node && a.port == b.port;
  };
  return std::ranges::equal(running.nodes,
                            written.nodes,
                            [&](const VP::Node &a, const VP::Node &b) {
                              return a.name == b.name && a.recipe == b.recipe &&
                                     std::ranges::equal(a.params, b.params, same_param);
                            }) &&
         std::ranges::equal(running.connections,
                            written.connections,
                            [&](const VP::Connection &a, const VP::Connection &b) {
                              return a.name == b.name && same_end(a.from, b.from) &&
                                     std::ranges::equal(a.to, b.to, same_end);
                            });
}

} // namespace
