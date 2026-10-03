#include "runtime/Edits.h"

#include "runtime/Commands.h"
#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace VP {

namespace {

using Arguments = std::span<const std::string_view>;

constexpr std::string_view blanks = " \t\r";
// The folder at a view's top that holds the contracts its nodes share (RV05).
constexpr std::string_view contracts = "contracts";

std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

// A name joins a manifest's section headers, its endpoints, the command line and a
// folder's name, so it holds only what each of them leaves whole.
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

// Names joined by dots: the nodes a node is inside, then its own, as its folder is the
// path of their folders (RV08).
bool is_path(std::string_view text) {
  for (std::size_t dot = text.find('.'); dot != std::string_view::npos;
       dot = text.find('.')) {
    if (!is_name(text.substr(0, dot)))
      return false;
    text.remove_prefix(dot + 1);
  }
  return is_name(text);
}

void check_path(std::string_view what, std::string_view path) {
  if (!is_path(path))
    throw std::runtime_error(
        std::format("{} {} is not a name, or names joined by dots: a name holds letters, "
                    "digits, _ and -",
                    what,
                    path));
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
      std::format("unknown word {} in a node; its words are recipe, operator, file, "
                  "invocations, vertex_count, instance_count, param and log",
                  key));
}

// A node's files are what its folder holds (RV08), so no command names one.
[[noreturn]] void no_file(std::string_view node) {
  throw std::runtime_error(std::format("a node's files are what its folder holds: put a "
                                       "file in the folder of node {} to add it",
                                       node));
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

// A count left out is none, so a count given is at least 1.
void set_count(std::uint32_t &slot,
               std::string_view key,
               std::string_view value,
               std::string_view none) {
  if (slot != 0)
    throw std::runtime_error(std::format("{} is set twice", key));
  slot = whole_number(value);
  if (slot == 0)
    throw std::runtime_error(std::format("{} is at least 1; leave it out {}", key, none));
}

// A recipe of the library by its name, and for a drop's copy an @ and the fingerprint
// of what it copied, in hex.
bool is_recipe(std::string_view text) {
  const std::size_t at = text.find('@');
  if (!is_name(text.substr(0, at)))
    return false;
  if (at == std::string_view::npos)
    return true;
  const std::string_view print = text.substr(at + 1);
  return !print.empty() && std::ranges::all_of(print, [](char digit) {
    return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
  });
}

// A file of the node's own folder, so its name holds no folder.
bool is_file(std::string_view text) {
  return text != "." && text != ".." &&
         text.find_first_of("/\\") == std::string_view::npos;
}

// A file of the node's folder, which a manifest lists once.
void add_file(Node &node, std::string_view value) {
  if (!is_file(value))
    throw std::runtime_error(std::format(
        "file {}: a file of the node's folder, named without a folder", value));
  if (std::ranges::find(node.files, value) != node.files.end())
    throw std::runtime_error(std::format("file {} is listed twice", value));
  node.files.emplace_back(value);
}

// `name=value`, whose name is a path when it sets a node inside a recipe the node uses.
void add_param(Node &node, std::string_view value) {
  const std::size_t equals = value.find('=');
  if (equals == std::string_view::npos)
    throw std::runtime_error("a param is written `param = name=value`");
  const std::string_view name = trim(value.substr(0, equals));
  const std::string_view setting = trim(value.substr(equals + 1));
  check_path("param", name);
  if (setting.empty())
    throw std::runtime_error(std::format("param {} has no value", name));
  if (std::ranges::find(node.params, name, &Param::key) != node.params.end())
    throw std::runtime_error(std::format("param {} is set twice", name));
  node.params.push_back({std::string(name), std::string(setting)});
}

// What no single word can show.
void check_whole(const Node &node) {
  if (node.uses()) {
    if (!node.operator_name.empty() || !node.files.empty() || node.invocations != 0 ||
        node.vertex_count != 0 || node.instance_count != 0)
      throw std::runtime_error(
          std::format("node {} uses recipe {} as it is, whose node gives it those words: "
                      "only param and log go with it",
                      node.name,
                      node.recipe));
    return;
  }
  for (const Param &param : node.params)
    if (!is_name(param.key))
      throw std::runtime_error(std::format(
          "param {}: a dotted key sets a node inside a recipe a node uses, and "
          "node {} uses none",
          param.key,
          node.name));
  if (node.invocations != 0 && node.vertex_count != 0)
    throw std::runtime_error(
        std::format("node {} counts invocations and vertex_count: a dispatch counts its "
                    "invocations, and a draw its vertex_count",
                    node.name));
  if (node.invocations != 0 && node.instance_count != 0)
    throw std::runtime_error(
        "instance_count counts a draw's instances; a dispatch has none");
}

Node *node_named(View &view, std::string_view name) {
  const auto found = std::ranges::find(view.nodes, name, &Node::name);
  return found == view.nodes.end() ? nullptr : &*found;
}

// The node that uses a recipe and so holds the named one, which the view as it runs
// holds once that recipe is unfolded (V11).
Node *user_of(View &view, std::string_view name) {
  for (std::size_t dot = name.find('.'); dot != std::string_view::npos;
       dot = name.find('.', dot + 1))
    if (Node *const node = node_named(view, name.substr(0, dot)); node && node->uses())
      return node;
  return nullptr;
}

Node &existing(View &view, std::string_view name) {
  if (Node *const node = node_named(view, name))
    return *node;
  if (const Node *const user = user_of(view, name))
    throw std::runtime_error(
        std::format("node {} is inside recipe {}, which node {} uses as it is: param set "
                    "and unset change its params, and the recipe's own view.vlp the rest",
                    name,
                    user->recipe,
                    user->name));
  throw std::runtime_error(std::format("no node is named {}", name));
}

// A node of the view, or one inside a recipe a node uses, which the view as it runs
// holds and checks.
bool has_node(View &view, std::string_view name) {
  return node_named(view, name) != nullptr || user_of(view, name) != nullptr;
}

// A param of a node inside a recipe a node uses: the user, and its params' key for it.
std::pair<Node &, std::string>
user_param(View &view, std::string_view node, std::string_view key) {
  Node *const user = user_of(view, node);
  if (!user)
    throw std::runtime_error(std::format("no node is named {}", node));
  return {*user, std::format("{}.{}", node.substr(user->name.size() + 1), key)};
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

// Before node set gives a word again: param words replace them all, and a word given
// empty stays clear.
void clear(Node &node, std::string_view key) {
  if (key == "recipe")
    node.recipe.clear();
  else if (key == "operator")
    node.operator_name.clear();
  else if (key == "invocations")
    node.invocations = 0;
  else if (key == "vertex_count")
    node.vertex_count = 0;
  else if (key == "instance_count")
    node.instance_count = 0;
  else if (key == "param")
    node.params.clear();
  else if (key == "log")
    node.log.clear();
  else if (key == "file")
    no_file(node.name);
  else
    unknown_word(key);
}

void node_add(View &view, Arguments arguments, std::string_view where) {
  Node node{.name = std::string(arguments.front()), .where = std::string(where)};
  for (const std::string_view word : arguments.subspan(1)) {
    const auto [key, value] = key_and_value(word);
    if (key == "file")
      no_file(node.name);
    Edits::word(node, key, value);
  }
  Edits::add(view, std::move(node));
}

// Its folder stays, as every file does until a person deletes it.
void node_remove(View &view, Arguments arguments, std::string_view) {
  // The argument, not the node's own name, which the erase below moves.
  const std::string_view name = arguments.front();
  existing(view, name);
  for (const Connection &connection : view.connections)
    if (connection.from.node == name ||
        std::ranges::find(connection.to, name, &Endpoint::node) != connection.to.end())
      throw std::runtime_error(std::format(
          "node {} is connected through {}; disconnect it first", name, connection.name));
  const std::string inside = std::string(name) + '.';
  for (const Node &node : view.nodes)
    if (node.name.starts_with(inside))
      throw std::runtime_error(
          std::format("node {} holds node {}; remove that first", name, node.name));
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

// A node inside a recipe a node uses keeps its params in that node, as the manifest
// writes them.
void param_set(View &view, Arguments arguments, std::string_view where) {
  check_name("param", arguments[1]);
  check_value(arguments[1], arguments[2]);
  if (!node_named(view, arguments[0]) && user_of(view, arguments[0])) {
    auto [user, key] = user_param(view, arguments[0], arguments[1]);
    set_param(user.params, key, arguments[2]);
    user.where = where;
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
  if (!node_named(view, arguments[0]) && user_of(view, arguments[0])) {
    auto [user, key] = user_param(view, arguments[0], arguments[1]);
    return unset(user.params, key, user.where);
  }
  Node &node = existing(view, arguments[0]);
  unset(node.params, arguments[1], node.where);
}

// The file arrives absolute, as the command port resolves a <file>.
void child_add(View &view, Arguments arguments, std::string_view where) {
  Edits::child(view,
               Child{.name = std::string(arguments[0]),
                     .file = std::filesystem::path(arguments[1]),
                     .where = std::string(where)});
}

void child_remove(View &view, Arguments arguments, std::string_view) {
  if (std::erase_if(view.children, [&](const Child &child) {
        return child.name == arguments.front();
      }) == 0)
    throw std::runtime_error(std::format(
        "this view hosts no view named {}; child list lists them", arguments.front()));
}

struct Edit {
  std::string_view usage;
  std::string_view help;
  // where: the file and line the edit comes from, which a node it changes keeps.
  void (*change)(View &view, Arguments arguments, std::string_view where);
};

// Each edit's usage and help (RV04), and what it changes.
constexpr std::array edits{
    Edit{"node add <name> [<word=value>...]",
         "adds a node, given the manifest's node words; its files are what its folder "
         "holds",
         node_add},
    Edit{
        "node remove <node>",
        "removes a node that no connection names and no node is inside; its folder stays",
        node_remove},
    Edit{"node set <node> <word=value>...",
         "gives a node the words named, clearing those given empty; param words replace "
         "them all",
         node_set},
    Edit{"connect <name> <port> <port>...",
         "joins the port that writes a buffer to the ports that read it",
         connect_ports},
    Edit{"disconnect <connection>", "removes a connection", disconnect},
    Edit{"param set <node> <key> <value>", "sets a param of a node", param_set},
    Edit{"param unset <node> <key>", "removes a param of a node", param_unset},
    Edit{"child add <name> <file>",
         "hosts the view a view.vlp holds, or an empty one that view save writes; a line "
         "`<name>: <command>` addresses it",
         child_add},
    Edit{"child remove <name>", "stops hosting a view; its files stay", child_remove},
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
    if (!is_recipe(value))
      throw std::runtime_error(
          std::format("recipe {}: a recipe's name, and on a drop's copy an @ and its "
                      "fingerprint, as probe@3f2a9c1e",
                      value));
    set_once(node.recipe, key, value);
  } else if (key == "operator") {
    set_once(node.operator_name, key, value);
  } else if (key == "file") {
    add_file(node, value);
  } else if (key == "log") {
    set_once(node.log, key, value);
  } else if (key == "invocations") {
    set_count(node.invocations, key, value, "on a node with no .comp");
  } else if (key == "vertex_count") {
    set_count(node.vertex_count, key, value, "on a node with no .vert");
  } else if (key == "instance_count") {
    set_count(node.instance_count, key, value, "for one");
  } else if (key == "param") {
    add_param(node, value);
  } else {
    unknown_word(key);
  }
}

// The port follows the last dot, so a node inside others keeps its own: ui.panel.rects.
Endpoint Edits::endpoint(std::string_view text) {
  const std::size_t dot = text.rfind('.');
  const std::string_view node = text.substr(0, dot);
  const std::string_view port =
      dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  if (!is_path(node) || !is_name(port))
    throw std::runtime_error(std::format("{} is not node.port", text));
  return {std::string(node), std::string(port)};
}

// A node's folder is its name's path in the view's folder (RV08), beside the folders of
// the views it hosts and of its contracts, so none of them shares a name.
void Edits::add(View &view, Node node) {
  check_path("node", node.name);
  if (node.name == contracts)
    throw std::runtime_error("contracts/ holds the contracts the view's nodes share "
                             "(RV05), so no node is named contracts");
  if (node_named(view, node.name) ||
      std::ranges::find(view.children, node.name, &Child::name) != view.children.end())
    throw std::runtime_error(std::format("two nodes or views are named {}", node.name));
  check_whole(node);
  view.nodes.push_back(std::move(node));
}

void Edits::child(View &view, Child child) {
  check_name("view", child.name);
  if (node_named(view, child.name) ||
      std::ranges::find(view.children, child.name, &Child::name) != view.children.end())
    throw std::runtime_error(std::format("two nodes or views are named {}", child.name));
  view.children.push_back(std::move(child));
}

void Edits::connect(View &view, Connection connection) {
  check_path("connection", connection.name);
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
//
// A hosted view's name is its own, so child remove reaches the view that hosts it from
// wherever the line goes, as from inside the view it closes.
void Edits::command(Call &call) {
  std::string address(_port.addressed());
  for (std::size_t index = 0; index < edits.size(); ++index)
    if (call.is(_commands[index]) && edits[index].change == child_remove)
      if (const std::optional<std::string> host =
              _views.host_of(call.arguments().front()))
        address = *host;
  const View *const found = _views.find(address);
  if (!found)
    throw std::runtime_error(std::format("no view is named {}", address));
  View edited = *found;
  for (std::size_t index = 0; index < edits.size(); ++index)
    if (call.is(_commands[index]))
      edits[index].change(edited, call.arguments(), _port.where());
  _views.replace(address, std::move(edited));
}

} // namespace VP
