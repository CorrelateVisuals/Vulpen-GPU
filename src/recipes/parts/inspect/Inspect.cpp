#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

std::string endpoint(const VP::Endpoint &end) {
  return std::format("{}.{}", end.node, end.port);
}

// Reads the graph and answers ls and info, so a view can be read without a window and a
// front end needs no mirror of the graph of its own.
class Inspect final : public VP::Operator {
  void bind(VP::Bind &node) override {
    _ls = node.command("ls", "lists the view's deploys, nodes and connections");
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
    for (const VP::Deploy &deploy : view.deploys)
      call.reply(std::format("deploy {} of recipe {}", deploy.name, deploy.recipe));
    for (const VP::Node &node : view.nodes)
      call.reply(std::format("node {} of recipe {}", node.name, node.recipe));
    for (const VP::Connection &connection : view.connections) {
      std::string to;
      for (const VP::Endpoint &end : connection.to)
        to.append(to.empty() ? "" : ", ").append(endpoint(end));
      call.reply(std::format(
          "connection {} from {} to {}", connection.name, endpoint(connection.from), to));
    }
  }

  // In the manifest's words, so what info shows is what a node section would hold.
  static void info(VP::Call &call, const VP::View &view, std::string_view name) {
    const auto node = std::ranges::find(view.nodes, name, &VP::Node::name);
    if (node == view.nodes.end())
      return deployed(call, view, name);
    call.reply(std::format("recipe = {}", node->recipe));
    if (!node->operator_name.empty())
      call.reply(std::format("operator = {}", node->operator_name));
    for (const std::string &shader : node->shaders)
      call.reply(std::format("shader = {}", shader));
    if (node->invocations != 0)
      call.reply(std::format("invocations = {}", node->invocations));
    for (const VP::Param &param : node->params)
      call.reply(std::format("param = {}={}", param.key, param.value));
    if (!node->log.empty())
      call.reply(std::format("log = {}", node->log));
    for (const VP::Connection &connection : view.connections) {
      if (connection.from.node == name)
        call.reply(std::format("writes {} as {}", connection.from.port, connection.name));
      for (const VP::Endpoint &end : connection.to)
        if (end.node == name)
          call.reply(
              std::format("reads {} from {}", end.port, endpoint(connection.from)));
    }
  }

  // A deploy's node lives in its recipe's view.vlp; the view holds only what the deploy
  // sets on it.
  static void deployed(VP::Call &call, const VP::View &view, std::string_view name) {
    const std::size_t dot = name.find('.');
    const auto deploy =
        std::ranges::find(view.deploys, name.substr(0, dot), &VP::Deploy::name);
    if (dot == std::string_view::npos || deploy == view.deploys.end())
      throw std::runtime_error(std::format("no node is named {}", name));
    const std::string node = std::format("{}.", name.substr(dot + 1));
    call.reply(
        std::format("comes from deploy {}, of recipe {}", deploy->name, deploy->recipe));
    for (const VP::Param &param : deploy->params)
      if (param.key.starts_with(node))
        call.reply(
            std::format("param = {}={}", param.key.substr(node.size()), param.value));
  }

  VP::Command _ls;
  VP::Command _info;
};

} // namespace

VP_RECIPE(registry) {
  registry.add<Inspect>("Inspect");
}
