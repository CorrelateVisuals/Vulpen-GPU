#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

// Reads the graph and answers ls and info, so a view can be read without a window and a
// front end needs no mirror of the graph of its own.
class Inspect final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _ls = node.command("ls",
                       "lists the views the view hosts, its nodes and its connections");
    _info = node.command("info <node>",
                         "shows a node's words, and the connections it writes and reads");
  }

  void command(VP::Call &call) override {
    if (call.is(_ls))
      ls(call, call.view());
    else if (call.is(_info))
      info(call, call.view(), call.arguments().front());
  }

  static void ls(VP::Call &call, const VP::View &view) {
    for (const VP::Child &child : view.children)
      call.reply(std::format("view {} from {}", child.name, child.file.string()));
    for (const VP::Node &node : view.nodes)
      call.reply(node.recipe.empty()
                     ? std::format("node {}", node.name)
                     : std::format("node {} of recipe {}", node.name, node.recipe));
    for (const VP::Connection &connection : view.connections) {
      std::string to;
      for (const VP::Endpoint &end : connection.to)
        to.append(to.empty() ? "" : ", ").append(end.text());
      call.reply(std::format(
          "connection {} from {} to {}", connection.name, connection.from.text(), to));
    }
  }

  // In the manifest's words, so what info shows is what a node section would hold.
  static void info(VP::Call &call, const VP::View &view, std::string_view name) {
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

  VP::Command _ls;
  VP::Command _info;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Inspect>("Inspect");
}
