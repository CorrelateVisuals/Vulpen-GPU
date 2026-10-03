#include "runtime/Edits.h"

#include "runtime/Commands.h"
#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace VP {

namespace {

using Arguments = std::span<const std::string_view>;

constexpr std::string_view blanks = " \t\r";

std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

// A name joins a manifest's section headers, its endpoints and the command line, so it
// holds only what each of them leaves whole.
bool is_name(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, [](char letter) {
    return (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
           (letter >= '0' && letter <= '9') || letter == '_' || letter == '-';
  });
}

void check_name(std::string_view what, std::string_view name) {
  if (!is_name(name))
    throw std::runtime_error(std::format(
        "{} {} is not a name: a name holds letters, digits, _ and -", what, name));
}

// A deploy's node goes by its deploy's name and its own: deploy.node.
bool deployed(std::string_view node) {
  return node.find('.') != std::string_view::npos;
}

// Names joined by dots, as a deploy names a node of its recipe: deploy.node.
bool is_path(std::string_view text) {
  for (std::size_t dot = text.find('.'); dot != std::string_view::npos;
       dot = text.find('.')) {
    if (!is_name(text.substr(0, dot)))
      return false;
    text.remove_prefix(dot + 1);
  }
  return is_name(text);
}

// A manifest reads # as the start of a comment and a line break as the end of a line, so
// a value that holds either would not survive a save. Blanks never reach a value: they
// split the words of a command, and a manifest trims them.
void check_value(std::string_view key, std::string_view value) {
  if (value.find_first_of("#\n") != std::string_view::npos)
    throw std::runtime_error(std::format(
        "{} {}: a value holds no # and no line break, which a manifest reads as a "
        "comment and a new line",
        key,
        value));
}

[[noreturn]] void unknown_word(std::string_view key) {
  throw std::runtime_error(
      std::format("unknown word {} in a node; its words are recipe, operator, shader, "
                  "invocations, instance_count, param and log",
                  key));
}

void set_once(std::string &slot, std::string_view key, std::string_view value) {
  if (!slot.empty())
    throw std::runtime_error(std::format("{} is set twice", key));
  slot = value;
}

std::uint32_t whole_number(std::string_view text) {
  std::uint32_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    throw std::runtime_error(std::format("{} is not a whole number", text));
  return value;
}

// What no single word can show.
void check_whole(const Node &node) {
  if (node.operator_name.empty() && node.shaders.empty())
    throw std::runtime_error(std::format(
        "node {} runs nothing: give it an operator, a shader or both", node.name));
  if (node.recipe.empty())
    throw std::runtime_error(
        std::format("node {} names no recipe to find its C++ and GLSL in", node.name));
  if (!node.shaders.empty() && node.invocations == 0)
    throw std::runtime_error(
        std::format("node {} runs a shader, so it needs invocations", node.name));
}

Node *node_named(View &view, std::string_view name) {
  const auto found = std::ranges::find(view.nodes, name, &Node::name);
  return found == view.nodes.end() ? nullptr : &*found;
}

Node &existing(View &view, std::string_view name) {
  if (Node *const node = node_named(view, name))
    return *node;
  if (deployed(name))
    throw std::runtime_error(std::format(
        "node {} comes from deploy {}: param set and unset change its params, and its "
        "recipe's view.vlp the rest",
        name,
        name.substr(0, name.find('.'))));
  throw std::runtime_error(std::format("no node is named {}", name));
}

Deploy *deploy_named(View &view, std::string_view name) {
  const auto found = std::ranges::find(view.deploys, name, &Deploy::name);
  return found == view.deploys.end() ? nullptr : &*found;
}

// A node of the view, or a node of a deploy, which the view the schedule runs holds
// once the deploys are unfolded, and which that view checks.
bool has_node(View &view, std::string_view name) {
  const std::size_t dot = name.find('.');
  return dot == std::string_view::npos
             ? node_named(view, name) != nullptr
             : deploy_named(view, name.substr(0, dot)) != nullptr;
}

// A param of a deploy's node: the deploy, and its params' key for it.
std::pair<Deploy &, std::string>
deploy_param(View &view, std::string_view node, std::string_view key) {
  const std::size_t dot = node.find('.');
  Deploy *const deploy = deploy_named(view, node.substr(0, dot));
  if (!deploy)
    throw std::runtime_error(std::format("no node is named {}", node));
  return {*deploy, std::format("{}.{}", node.substr(dot + 1), key)};
}

void set_param(std::vector<Param> &params, std::string_view key, std::string_view value) {
  const auto found = std::ranges::find(params, key, &Param::key);
  if (found != params.end())
    found->value = value;
  else
    params.push_back({std::string(key), std::string(value)});
}

bool same(const Endpoint &one, const Endpoint &other) {
  return one.node == other.node && one.port == other.port;
}

// A port holds one buffer, so it joins one connection, once.
void check_ports(const View &view, const Connection &connection) {
  std::vector<const Endpoint *> checked;
  const auto check = [&](const Endpoint &end) {
    const auto at = [&](const Endpoint &other) { return same(end, other); };
    const bool joined =
        std::ranges::any_of(checked, [&](const Endpoint *other) { return at(*other); }) ||
        std::ranges::any_of(view.connections, [&](const Connection &other) {
          return at(other.from) || std::ranges::any_of(other.to, at);
        });
    if (joined)
      throw std::runtime_error(
          std::format("{}.{} joins two connections, or one twice", end.node, end.port));
    checked.push_back(&end);
  };
  check(connection.from);
  for (const Endpoint &to : connection.to)
    check(to);
}

// Whether a node reads, through connections, what another writes. A node may read
// its own buffer, which orders nothing.
bool reaches(const View &view, std::string_view writer, std::string_view reader) {
  // A node that writes nothing yet reaches nothing, as each node of a manifest does
  // while it loads, so its connections cost no index.
  if (std::ranges::none_of(view.connections, [&](const Connection &connection) {
        return connection.from.node == writer;
      }))
    return false;
  std::multimap<std::string_view, std::string_view> readers;
  for (const Connection &connection : view.connections)
    for (const Endpoint &to : connection.to)
      if (to.node != connection.from.node)
        readers.emplace(connection.from.node, to.node);
  std::vector<std::string_view> left{writer};
  std::set<std::string_view> seen;
  while (!left.empty()) {
    const std::string_view node = left.back();
    left.pop_back();
    if (node == reader)
      return true;
    if (!seen.insert(node).second)
      continue;
    for (auto [at, end] = readers.equal_range(node); at != end; ++at)
      left.push_back(at->second);
  }
  return false;
}

// A word=value argument, split at its first =.
std::pair<std::string_view, std::string_view> key_and_value(std::string_view word) {
  const std::size_t equals = word.find('=');
  if (equals == std::string_view::npos)
    throw std::runtime_error(std::format("{} is not word=value", word));
  return {word.substr(0, equals), word.substr(equals + 1)};
}

// Before node set gives a word again: shader and param words replace them all, and a
// word given empty stays clear.
void clear(Node &node, std::string_view key) {
  if (key == "recipe")
    node.recipe.clear();
  else if (key == "operator")
    node.operator_name.clear();
  else if (key == "shader")
    node.shaders.clear();
  else if (key == "invocations")
    node.invocations = 0;
  else if (key == "instance_count")
    node.instance_count = 0;
  else if (key == "param")
    node.params.clear();
  else if (key == "log")
    node.log.clear();
  else
    unknown_word(key);
}

void node_add(View &view, Arguments arguments, std::string_view where) {
  Node node{.name = std::string(arguments.front()), .where = std::string(where)};
  for (const std::string_view word : arguments.subspan(1)) {
    const auto [key, value] = key_and_value(word);
    Edits::word(node, key, value);
  }
  Edits::add(view, std::move(node));
}

void node_remove(View &view, Arguments arguments, std::string_view) {
  // The argument, not the node's own name, which the erase below moves.
  const std::string_view name = arguments.front();
  existing(view, name);
  for (const Connection &connection : view.connections)
    if (connection.from.node == name ||
        std::ranges::find(connection.to, name, &Endpoint::node) != connection.to.end())
      throw std::runtime_error(std::format(
          "node {} is connected through {}; disconnect it first", name, connection.name));
  std::erase_if(view.nodes, [&](const Node &node) { return node.name == name; });
}

void node_set(View &view, Arguments arguments, std::string_view where) {
  Node &node = existing(view, arguments.front());
  Node changed = node;
  changed.where = where;
  const Arguments words = arguments.subspan(1);
  for (const std::string_view word : words)
    clear(changed, key_and_value(word).first);
  for (const std::string_view word : words)
    if (const auto [key, value] = key_and_value(word); !value.empty())
      Edits::word(changed, key, value);
  check_whole(changed);
  node = std::move(changed);
}

// A connection's errors name the connection, so it leaves the nodes' lines alone.
void connect_ports(View &view, Arguments arguments, std::string_view) {
  Connection connection{.name = std::string(arguments[0]),
                        .from = Edits::endpoint(arguments[1])};
  for (const std::string_view to : arguments.subspan(2))
    connection.to.push_back(Edits::endpoint(to));
  Edits::connect(view, std::move(connection));
}

void disconnect(View &view, Arguments arguments, std::string_view) {
  if (std::erase_if(view.connections, [&](const Connection &connection) {
        return connection.name == arguments.front();
      }) == 0)
    throw std::runtime_error(std::format("no connection is named {}", arguments.front()));
}

// A deploy's node keeps its params in the deploy, as the manifest writes them.
void param_set(View &view, Arguments arguments, std::string_view where) {
  check_name("param", arguments[1]);
  check_value(arguments[1], arguments[2]);
  if (deployed(arguments[0])) {
    auto [deploy, key] = deploy_param(view, arguments[0], arguments[1]);
    set_param(deploy.params, key, arguments[2]);
    deploy.where = where;
    return;
  }
  Node &node = existing(view, arguments[0]);
  set_param(node.params, arguments[1], arguments[2]);
  node.where = where;
}

void param_unset(View &view, Arguments arguments, std::string_view where) {
  const auto unset = [&](std::vector<Param> &params,
                         std::string_view key,
                         std::string &by) {
    if (std::erase_if(params, [&](const Param &param) { return param.key == key; }) == 0)
      throw std::runtime_error(
          std::format("node {} sets no param {}", arguments[0], arguments[1]));
    by = where;
  };
  if (deployed(arguments[0])) {
    auto [deploy, key] = deploy_param(view, arguments[0], arguments[1]);
    return unset(deploy.params, key, deploy.where);
  }
  Node &node = existing(view, arguments[0]);
  unset(node.params, arguments[1], node.where);
}

void deploy_add(View &view, Arguments arguments, std::string_view where) {
  Deploy deploy{.name = std::string(arguments[0]), .where = std::string(where)};
  Edits::word(deploy, "recipe", arguments[1]);
  Edits::deploy(view, std::move(deploy));
}

void deploy_remove(View &view, Arguments arguments, std::string_view) {
  const std::string_view name = arguments.front();
  if (!deploy_named(view, name))
    throw std::runtime_error(std::format("no deploy is named {}", name));
  const auto inside = [&](const Endpoint &end) {
    return end.node.starts_with(name) && end.node.size() > name.size() &&
           end.node[name.size()] == '.';
  };
  for (const Connection &connection : view.connections)
    if (inside(connection.from) || std::ranges::any_of(connection.to, inside))
      throw std::runtime_error(
          std::format("deploy {} is connected through {}; disconnect it first",
                      name,
                      connection.name));
  std::erase_if(view.deploys, [&](const Deploy &deploy) { return deploy.name == name; });
}

struct Edit {
  std::string_view usage;
  std::string_view help;
  // where: the file and line the edit comes from, which a node it changes keeps.
  void (*change)(View &view, Arguments arguments, std::string_view where);
};

// Each edit's usage and help (RV04), and what it changes.
constexpr std::array edits{
    Edit{"node add <name> <word=value>...",
         "adds a node, given the manifest's node words",
         node_add},
    Edit{"node remove <node>", "removes a node that no connection names", node_remove},
    Edit{"node set <node> <word=value>...",
         "gives a node the words named, clearing those given empty; shader and param "
         "words replace them all",
         node_set},
    Edit{"connect <name> <port> <port>...",
         "joins the port that writes a buffer to the ports that read it",
         connect_ports},
    Edit{"disconnect <connection>", "removes a connection", disconnect},
    Edit{"param set <node> <key> <value>", "sets a param of a node", param_set},
    Edit{"param unset <node> <key>", "removes a param of a node", param_unset},
    Edit{"deploy add <name> <recipe>",
         "deploys a recipe of the view under a name: its nodes, as <name>.<node>",
         deploy_add},
    Edit{"deploy remove <deploy>",
         "removes a deploy that no connection names",
         deploy_remove},
};

} // namespace

Edits::Edits(Commands &commands, ViewLookup &views) : _port(commands), _views(views) {
  for (const Edit &edit : edits)
    _commands.push_back(commands.add(edit.usage, edit.help, *this, Primitive::yes));
}

void Edits::word(Node &node, std::string_view key, std::string_view value) {
  if (value.empty())
    throw std::runtime_error(std::format("{} has no value", key));
  check_value(key, value);
  if (key == "recipe") {
    set_once(node.recipe, key, value);
  } else if (key == "operator") {
    set_once(node.operator_name, key, value);
  } else if (key == "shader") {
    node.shaders.emplace_back(value);
  } else if (key == "log") {
    set_once(node.log, key, value);
  } else if (key == "invocations") {
    if (node.invocations != 0)
      throw std::runtime_error("invocations is set twice");
    node.invocations = whole_number(value);
  } else if (key == "instance_count") {
    if (node.instance_count != 0)
      throw std::runtime_error("instance_count is set twice");
    node.instance_count = whole_number(value);
    if (node.instance_count == 0)
      throw std::runtime_error("instance_count is at least 1; leave it out for one");
  } else if (key == "param") {
    const std::size_t equals = value.find('=');
    if (equals == std::string_view::npos)
      throw std::runtime_error("a param is written `param = name=value`");
    const std::string_view name = trim(value.substr(0, equals));
    const std::string_view setting = trim(value.substr(equals + 1));
    check_name("param", name);
    if (setting.empty())
      throw std::runtime_error(std::format("param {} has no value", name));
    if (std::ranges::find(node.params, name, &Param::key) != node.params.end())
      throw std::runtime_error(std::format("param {} is set twice", name));
    node.params.push_back({std::string(name), std::string(setting)});
  } else {
    unknown_word(key);
  }
}

void Edits::word(Deploy &deploy, std::string_view key, std::string_view value) {
  if (value.empty())
    throw std::runtime_error(std::format("{} has no value", key));
  check_value(key, value);
  if (key == "recipe") {
    check_name("recipe", value);
    set_once(deploy.recipe, key, value);
  } else if (key == "param") {
    const std::size_t equals = value.find('=');
    const std::string_view name = trim(value.substr(0, equals));
    const std::string_view setting = equals == std::string_view::npos
                                         ? std::string_view{}
                                         : trim(value.substr(equals + 1));
    if (!deployed(name) || !is_path(name) || setting.empty())
      throw std::runtime_error("a deploy's param is written `param = node.name=value`");
    if (std::ranges::find(deploy.params, name, &Param::key) != deploy.params.end())
      throw std::runtime_error(std::format("param {} is set twice", name));
    deploy.params.push_back({std::string(name), std::string(setting)});
  } else {
    throw std::runtime_error(
        std::format("unknown word {} in a deploy; its words are recipe and param", key));
  }
}

// The port follows the last dot, so a deploy's node keeps its own: deploy.node.port.
Endpoint Edits::endpoint(std::string_view text) {
  const std::size_t dot = text.rfind('.');
  const std::string_view node = text.substr(0, dot);
  const std::string_view port =
      dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  if (!is_path(node) || !is_name(port))
    throw std::runtime_error(std::format("{} is not node.port", text));
  return {std::string(node), std::string(port)};
}

// A deploy's nodes are named <deploy>.<node>, so a node and a deploy never share a name.
void Edits::add(View &view, Node node) {
  check_name("node", node.name);
  if (node_named(view, node.name) || deploy_named(view, node.name))
    throw std::runtime_error(std::format("two nodes or deploys are named {}", node.name));
  check_whole(node);
  view.nodes.push_back(std::move(node));
}

void Edits::deploy(View &view, Deploy deploy) {
  check_name("deploy", deploy.name);
  if (node_named(view, deploy.name) || deploy_named(view, deploy.name))
    throw std::runtime_error(
        std::format("two nodes or deploys are named {}", deploy.name));
  if (deploy.recipe.empty())
    throw std::runtime_error(
        std::format("deploy {} names no recipe to deploy", deploy.name));
  view.deploys.push_back(std::move(deploy));
}

void Edits::connect(View &view, Connection connection) {
  check_name("connection", connection.name);
  if (std::ranges::find(view.connections, connection.name, &Connection::name) !=
      view.connections.end())
    throw std::runtime_error(
        std::format("two connections are named {}", connection.name));
  if (connection.from.node.empty() || connection.to.empty())
    throw std::runtime_error(
        std::format("connection {} needs a from and at least one to", connection.name));
  if (!has_node(view, connection.from.node))
    throw std::runtime_error(std::format("connection {} is from {}, which is no node",
                                         connection.name,
                                         connection.from.node));
  for (const Endpoint &to : connection.to)
    if (!has_node(view, to.node))
      throw std::runtime_error(std::format(
          "connection {} is to {}, which is no node", connection.name, to.node));
  check_ports(view, connection);
  for (const Endpoint &to : connection.to)
    if (to.node != connection.from.node && reaches(view, to.node, connection.from.node))
      throw std::runtime_error(std::format(
          "connection {} would close a cycle through {}", connection.name, to.node));
  view.connections.push_back(std::move(connection));
}

// The schedule takes the edited view before the next frame, once for all the edits
// since the last, so it sees only where they end: a node removed and added again in
// between keeps its operator and buffers, as it does across a re-read of the manifest.
void Edits::command(Call &call) {
  View edited = call.view();
  for (std::size_t index = 0; index < edits.size(); ++index)
    if (call.is(_commands[index]))
      edits[index].change(edited, call.arguments(), _port.where());
  _views.replace(_port.addressed(), std::move(edited));
}

} // namespace VP
