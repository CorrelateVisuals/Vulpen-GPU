#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view note_gap = "  "; // between a folder and what it is
constexpr std::size_t bar_room = 10;        // a bar's #, for the costliest beside it
// Half a microsecond, in milliseconds: a time below it prints as 0.000, so it has no bar.
constexpr float least_shown = 0.0005f;
constexpr std::size_t no_node = static_cast<std::size_t>(-1);

// A row ls answers: a folder or a file, indented under the folder it is in, and what
// the folder is, empty for none.
using Rows = std::vector<std::pair<std::string, std::string>>;

// What a node and the nodes inside it cost together, in milliseconds a frame, smoothed:
// whether any of them cooks or has a pass, and whether a pass of theirs went untimed.
struct Sum {
  float cpu = 0.0f;
  float gpu = 0.0f;
  bool cooks = false;
  bool runs = false;
  bool untimed = false;
};

// What the nodes of a view cost, as probe stats prints them: each node's parent, the
// nodes inside each, the roots last, and each node's sum.
struct Tree {
  std::vector<std::size_t> parents;
  std::vector<std::vector<std::size_t>> inside;
  std::vector<Sum> sums;
};

// A node's parent is the node whose name its own starts with, up to a dot, the longest
// such, as a view names the nodes inside a node (RV08).
Tree tree_of(std::span<const VP::Cost> costs) {
  Tree tree{.parents = std::vector<std::size_t>(costs.size(), no_node),
            .inside = std::vector<std::vector<std::size_t>>(costs.size() + 1),
            .sums = std::vector<Sum>(costs.size())};
  for (std::size_t at = 0; at < costs.size(); ++at) {
    const std::string_view name = costs[at].node;
    for (std::size_t dot = name.rfind('.');
         dot != std::string_view::npos && tree.parents[at] == no_node;
         dot = dot == 0 ? std::string_view::npos : name.rfind('.', dot - 1))
      for (std::size_t other = 0; other < costs.size(); ++other)
        if (costs[other].node == name.substr(0, dot))
          tree.parents[at] = other;
    tree.inside[tree.parents[at] == no_node ? costs.size() : tree.parents[at]].push_back(at);
    for (std::size_t up = at; up != no_node; up = tree.parents[up]) {
      Sum &sum = tree.sums[up];
      sum.cpu += costs[at].cooks ? costs[at].cpu : 0.0f;
      sum.gpu += costs[at].timed ? costs[at].gpu : 0.0f;
      sum.cooks = sum.cooks || costs[at].cooks;
      sum.runs = sum.runs || costs[at].runs;
      sum.untimed = sum.untimed || (costs[at].runs && !costs[at].timed);
    }
  }
  return tree;
}

// A time and a bar of # as long as its share of the most, or blanks for none; ? for a
// pass the GPU did not time.
std::string column(float milliseconds, float most, bool has, bool untimed) {
  if (!has)
    return std::string(8 + bar_room, ' ');
  if (untimed && milliseconds == 0.0f)
    return std::format("{:>7} {:<{}}", "?", "", bar_room);
  const auto bars = milliseconds >= least_shown
                        ? static_cast<std::size_t>(std::lround(
                              static_cast<float>(bar_room) * milliseconds / most))
                        : 0;
  return std::format("{:7.3f} {:<{}}", milliseconds, std::string(bars, '#'), bar_room);
}

// Reads the graph and answers ls and info, so a view can be read without a window and a
// front end needs no mirror of the graph of its own. info with no node asks for the nodes
// selected through selected, the command a part that selects registers, as the graph
// does, so it reads a selection without a connection, which the CLI could not give it.
class Inspect final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _ls = node.command("ls",
                       "lists the view as folders: each node with its files, and each "
                       "view it hosts");
    _info = node.command("info [<node>]",
                         "shows a node's words, or a hosted view's, and the connections "
                         "it writes and reads; with none, each selected one's under its "
                         "header");
    _stats = node.command("probe stats [<node>]",
                          "prints what each node costs a frame on the CPU and the GPU, in "
                          "milliseconds smoothed over about a second, as a tree, a node "
                          "holding others counting theirs, each bar to the costliest "
                          "beside it; with a node, that node's");
  }

  void command(VP::Call &call) override {
    if (call.is(_stats))
      stats(call, call.arguments().empty() ? std::string_view() : call.arguments().front());
    else if (call.is(_ls))
      ls(call, call.view());
    else if (call.is(_info) && !call.arguments().empty())
      info(call, call.view(), call.arguments().front());
    else if (call.is(_info))
      info_selected(call);
  }

  // Each as the manifest's section of it would read, header first: a node's, or a view's
  // the view hosts.
  static void info_selected(VP::Call &call) {
    const std::vector<VP::Usage> usages = call.commands().usages();
    if (std::ranges::find(usages, "selected", &VP::Usage::usage) == usages.end())
      throw std::runtime_error("nothing here selects nodes; info <node> shows one");
    const std::string selected = call.commands().send("selected");
    if (selected.empty())
      throw std::runtime_error("no node is selected; info <node> shows one");
    for (std::size_t at = 0; at < selected.size();) {
      const std::size_t end = std::min(selected.find('\n', at), selected.size());
      const std::string node = selected.substr(at, end - at);
      const std::vector<VP::Child> &hosted = call.view().children;
      const bool view = std::ranges::find(hosted, node, &VP::Child::name) != hosted.end();
      call.reply(std::format("[{} \"{}\"]", view ? "view" : "node", node));
      info(call, call.view(), node);
      at = end + 1;
    }
  }

  // The view as the folders its nodes are (RV08): each node a folder under the node it is
  // inside, its files under it and beside it the recipe it uses or was dropped from, and
  // each view it hosts a folder, beside it the file it is hosted from. Connections are
  // each node's, which info shows. What each folder is reads down one column.
  static void ls(VP::Call &call, const VP::View &view) {
    Rows rows;
    for (const VP::Child &child : view.children)
      rows.emplace_back(child.name + '/',
                        std::format("view, from {}", child.file.string()));
    for (const VP::Node &node : view.nodes)
      if (node.name.find('.') == std::string::npos)
        list(view, node, 0, rows);
    std::size_t width = 0;
    for (const auto &[folder, note] : rows)
      if (!note.empty())
        width = std::max(width, folder.size());
    for (const auto &[folder, note] : rows)
      call.reply(note.empty() ? folder
                              : std::string(folder)
                                    .append(width - folder.size(), ' ')
                                    .append(note_gap)
                                    .append(note));
  }

  // A node's folder, its files, then the folders of the nodes inside it, each in the
  // manifest's order.
  static void
  list(const VP::View &view, const VP::Node &node, std::size_t depth, Rows &rows) {
    const std::string indent(2 * depth, ' ');
    const std::string_view name =
        std::string_view(node.name).substr(node.name.rfind('.') + 1);
    rows.emplace_back(indent + std::string(name) + '/',
                      node.recipe.empty() ? "" : std::format("recipe {}", node.recipe));
    for (const std::string &file : node.files)
      rows.emplace_back(indent + "  " + file, "");
    const std::string inside = node.name + '.';
    for (const VP::Node &inner : view.nodes)
      if (inner.name.starts_with(inside) &&
          inner.name.find('.', inside.size()) == std::string::npos)
        list(view, inner, depth + 1, rows);
  }

  // In the manifest's words, so what info shows is what a node section would hold, or a
  // view section, its file whole, with the connections that take its window.
  static void info(VP::Call &call, const VP::View &view, std::string_view name) {
    if (const auto child = std::ranges::find(view.children, name, &VP::Child::name);
        child != view.children.end()) {
      call.reply(std::format("file = {}", child->file.string()));
      for (const VP::Connection &connection : view.connections)
        if (connection.from.view() && connection.from.node == name)
          call.reply(std::format("writes its window as {}", connection.name));
      return;
    }
    const auto node = std::ranges::find(view.nodes, name, &VP::Node::name);
    if (node == view.nodes.end())
      return inside(call, view, name);
    if (!node->recipe.empty())
      call.reply(std::format("recipe = {}", node->recipe));
    if (!node->operator_name.empty())
      call.reply(std::format("operator = {}", node->operator_name));
    for (const std::string &file : node->files)
      call.reply(std::format("file = {}", file));
    if (node->invocations != 0)
      call.reply(std::format("invocations = {}", node->invocations));
    if (node->vertex_count != 0)
      call.reply(std::format("vertex_count = {}", node->vertex_count));
    if (!node->instance_count.empty())
      call.reply(std::format("instance_count = {}", node->instance_count));
    for (const VP::Param &param : node->params)
      call.reply(std::format("param = {}={}", param.key, param.value));
    for (const VP::ImagePort &image : node->images)
      call.reply(std::format("image = {}={}", image.port, image.format));
    if (!node->log.empty())
      call.reply(std::format("log = {}", node->log));
    for (const VP::Connection &connection : view.connections) {
      if (connection.from.node == name)
        call.reply(std::format("writes {} as {}", connection.from.port, connection.name));
      for (const VP::Endpoint &end : connection.to)
        if (end.node == name)
          call.reply(
              std::format("reads {} from {}", end.port, connection.from.text()));
    }
  }

  // A node inside a recipe a node uses lives in the recipe's view.vlp; the view holds
  // only the params that node sets on it.
  static void inside(VP::Call &call, const VP::View &view, std::string_view name) {
    for (std::size_t dot = name.find('.'); dot != std::string_view::npos;
         dot = name.find('.', dot + 1)) {
      const auto user =
          std::ranges::find(view.nodes, name.substr(0, dot), &VP::Node::name);
      if (user == view.nodes.end() || !user->uses())
        continue;
      call.reply(std::format(
          "inside recipe {}, which node {} uses as it is", user->recipe, user->name));
      const std::string prefix = std::format("{}.", name.substr(dot + 1));
      for (const VP::Param &param : user->params)
        if (param.key.starts_with(prefix))
          call.reply(
              std::format("param = {}={}", param.key.substr(prefix.size()), param.value));
      return;
    }
    throw std::runtime_error(std::format("no node is named {}", name));
  }

  // The view as it runs, each node a row under the node it is inside, as ls lists them:
  // its cost on the CPU and on the GPU, each with its bar, then the frame's. A node in
  // a recipe a node uses is the using node's.
  static void stats(VP::Call &call, std::string_view under) {
    const std::span<const VP::Cost> costs = call.costs();
    if (costs.empty())
      throw std::runtime_error("the view runs no node yet");
    const Tree tree = tree_of(costs);
    std::vector<std::size_t> roots = tree.inside.back();
    if (!under.empty()) {
      const auto found = std::ranges::find(costs, under, &VP::Cost::node);
      if (found == costs.end())
        throw std::runtime_error(
            std::format("no node named {} runs in the view; probe stats lists them", under));
      roots.assign(1, static_cast<std::size_t>(found - costs.begin()));
    }
    Rows rows;
    for (const std::size_t root : roots)
      cost_rows(costs, tree, root, roots, 0, rows);
    std::size_t width = 0;
    for (const auto &[name, columns] : rows)
      width = std::max(width, name.size());
    call.reply(std::format("{:<{}}  {:<{}} {}", "ms a frame", width, "cpu", 8 + bar_room, "gpu"));
    for (const auto &[name, columns] : rows)
      call.reply(std::format("{:<{}}  {}", name, width, columns));
    Sum frame;
    for (const std::size_t root : roots) {
      frame.cpu += tree.sums[root].cpu;
      frame.gpu += tree.sums[root].gpu;
      frame.runs = frame.runs || tree.sums[root].runs;
      frame.untimed = frame.untimed || tree.sums[root].untimed;
    }
    call.reply(std::format(
        "in all: {:.3f} ms on the CPU{}{}",
        frame.cpu,
        frame.runs ? std::format(", {:.3f} ms on the GPU", frame.gpu) : std::string(),
        frame.untimed ? ", a ? marking a pass the GPU did not time" : ""));
  }

  // A node's row, then the rows of the nodes inside it, each bar to the costliest of those
  // beside it; a node that neither cooks nor runs a pass, nor holds one that does, as
  // one holding only files, costs nothing and has none.
  static void cost_rows(std::span<const VP::Cost> costs,
                        const Tree &tree,
                        std::size_t at,
                        std::span<const std::size_t> beside,
                        std::size_t depth,
                        Rows &rows) {
    if (!tree.sums[at].cooks && !tree.sums[at].runs)
      return;
    float most_cpu = 0.0f;
    float most_gpu = 0.0f;
    for (const std::size_t other : beside) {
      most_cpu = std::max(most_cpu, tree.sums[other].cpu);
      most_gpu = std::max(most_gpu, tree.sums[other].gpu);
    }
    const std::string_view name = costs[at].node;
    const Sum &sum = tree.sums[at];
    rows.emplace_back(
        std::string(2 * depth, ' ')
            .append(depth == 0 ? name : name.substr(name.rfind('.') + 1)),
        column(sum.cpu, most_cpu, sum.cooks, false) + ' ' +
            column(sum.gpu, most_gpu, sum.runs, sum.untimed));
    for (const std::size_t inner : tree.inside[at])
      cost_rows(costs, tree, inner, tree.inside[at], depth + 1, rows);
  }

  VP::Command _ls;
  VP::Command _info;
  VP::Command _stats;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Inspect>("Inspect");
}
