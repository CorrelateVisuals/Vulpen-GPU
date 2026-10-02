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
                  "invocations, param and log",
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
  throw std::runtime_error(std::format("no node is named {}", name));
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

void param_set(View &view, Arguments arguments, std::string_view where) {
  Node &node = existing(view, arguments[0]);
  check_name("param", arguments[1]);
  check_value(arguments[1], arguments[2]);
  node.where = where;
  const auto found = std::ranges::find(node.params, arguments[1], &Param::key);
  if (found != node.params.end())
    found->value = arguments[2];
  else
    node.params.push_back({std::string(arguments[1]), std::string(arguments[2])});
}

void param_unset(View &view, Arguments arguments, std::string_view where) {
  Node &node = existing(view, arguments[0]);
  if (std::erase_if(node.params,
                    [&](const Param &param) { return param.key == arguments[1]; }) == 0)
    throw std::runtime_error(
        std::format("node {} sets no param {}", node.name, arguments[1]));
  node.where = where;
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

Endpoint Edits::endpoint(std::string_view text) {
  const std::size_t dot = text.find('.');
  const std::string_view node = text.substr(0, dot);
  const std::string_view port =
      dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  if (!is_name(node) || !is_name(port))
    throw std::runtime_error(std::format("{} is not node.port", text));
  return {std::string(node), std::string(port)};
}

void Edits::add(View &view, Node node) {
  check_name("node", node.name);
  if (node_named(view, node.name))
    throw std::runtime_error(std::format("two nodes are named {}", node.name));
  check_whole(node);
  view.nodes.push_back(std::move(node));
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
  if (!node_named(view, connection.from.node))
    throw std::runtime_error(std::format("connection {} is from {}, which is no node",
                                         connection.name,
                                         connection.from.node));
  for (const Endpoint &to : connection.to)
    if (!node_named(view, to.node))
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
  const View *const view = _views.find({});
  if (!view)
    throw std::runtime_error("no view to edit");
  View edited = *view;
  for (std::size_t index = 0; index < edits.size(); ++index)
    if (call.is(_commands[index]))
      edits[index].change(edited, call.arguments(), _port.where());
  _views.replace({}, std::move(edited));
}

} // namespace VP
