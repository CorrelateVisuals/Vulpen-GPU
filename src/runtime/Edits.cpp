#include "runtime/Edits.h"

#include "runtime/Commands.h"
#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <filesystem>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace VP {

namespace {

using Arguments = std::span<const std::string_view>;

// What an edit knows besides the view and its words: the file and line it comes from,
// which a node it changes keeps, and the views hosted beside the view, whose windows a
// connection may take as it takes those of the views the view hosts.
struct Around {
  std::string_view where;
  std::span<const std::string> beside;
};

constexpr std::string_view blanks = " \t\r";

std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

void check_name(std::string_view what, std::string_view name) {
  if (!is_name(name))
    throw std::runtime_error(std::format(
        "{} {} is not a name: a name holds letters, digits, _ and -", what, name));
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
  std::string words;
  for (const std::string_view word : node_words)
    words.append(words.empty() ? "" : word == node_words.back() ? " and " : ", ")
        .append(word);
  throw std::runtime_error(
      std::format("unknown word {} in a node; its words are {}", key, words));
}

// A node's files are what its folder holds (RV08), so no command or manifest names one.
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
  if (end == text.data() + text.size() && error == std::errc::result_out_of_range)
    throw std::runtime_error(std::format("{} is more than the largest count, {}",
                                         text,
                                         std::numeric_limits<std::uint32_t>::max()));
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

// A draw's instances: a number of at least 1, or the port of a buffer its shaders read,
// whose used length counts them each frame.
void set_instances(std::string &slot, std::string_view value) {
  const bool number = value.front() >= '0' && value.front() <= '9';
  if (number && whole_number(value) == 0)
    throw std::runtime_error("instance_count is at least 1; leave it out for one");
  if (!number && !is_name(value))
    throw std::runtime_error(std::format(
        "instance_count {}: a number, or the port of a buffer that counts them", value));
  set_once(slot, "instance_count", value);
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

// `name=value`, whose name is a path when it sets a node inside a recipe the node uses.
void add_param(Node &node, std::string_view value) {
  const std::size_t equals = value.find('=');
  if (equals == std::string_view::npos)
    throw std::runtime_error(std::format(
        "param {} names no param: a param is name=value, so a line gives it as "
        "param=tint=1,1,1, or alone as param set <node> tint 1,1,1, and a manifest as "
        "param = tint=1,1,1",
        value));
  const std::string_view name = trim(value.substr(0, equals));
  const std::string_view setting = trim(value.substr(equals + 1));
  check_path("param", name);
  if (setting.empty())
    throw std::runtime_error(std::format("param {} has no value", name));
  if (std::ranges::find(node.params, name, &Param::key) != node.params.end())
    throw std::runtime_error(std::format("param {} is set twice", name));
  node.params.push_back({std::string(name), std::string(setting)});
}

// `port=FORMAT`. Which formats there are, the loader knows, as it knows the log levels.
void add_image(Node &node, std::string_view value) {
  const std::size_t equals = value.find('=');
  if (equals == std::string_view::npos)
    throw std::runtime_error(
        "an image is written `image = port=FORMAT`, as image = atlas=R8_UNORM");
  const std::string_view port = trim(value.substr(0, equals));
  const std::string_view format = trim(value.substr(equals + 1));
  check_name("image", port);
  if (format.empty())
    throw std::runtime_error(std::format("image {} has no format", port));
  if (std::ranges::find(node.images, port, &ImagePort::port) != node.images.end())
    throw std::runtime_error(std::format("image {} is set twice", port));
  node.images.push_back({std::string(port), std::string(format)});
}

// What no single word can show.
void check_whole(const Node &node) {
  if (node.uses()) {
    if (!node.operator_name.empty() || node.invocations != 0 || node.vertex_count != 0 ||
        !node.instance_count.empty() || !node.images.empty())
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
  if (node.invocations != 0 && !node.instance_count.empty())
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
          std::format("{} joins two connections, or one twice", end.text()));
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

// Before node set gives a word again: param and image words replace them all, and a
// word given empty stays clear.
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
    node.instance_count.clear();
  else if (key == "param")
    node.params.clear();
  else if (key == "image")
    node.images.clear();
  else if (key == "log")
    node.log.clear();
  else if (key == "file")
    no_file(node.name);
  else
    unknown_word(key);
}

void node_add(View &view, Arguments arguments, const Around &around) {
  Node node{.name = std::string(arguments.front()), .where = std::string(around.where)};
  for (const std::string_view word : arguments.subspan(1)) {
    const auto [key, value] = key_and_value(word);
    Edits::word(node, key, value);
  }
  Edits::add(view, std::move(node));
}

// The node goes whole, with the nodes inside it: a connection one of them writes goes,
// and one they read stays with the readers that stay, so one line removes it, which a
// replay expands alike (V08). Their folders stay, as every file does until a person
// deletes it.
void node_remove(View &view, Arguments arguments, const Around &) {
  // A copy of the argument, not the node's own name, which the erase below moves.
  const std::string name(arguments.front());
  existing(view, name);
  const auto ends_here = [&](const Endpoint &end) {
    return !end.view() && within(end.node, name);
  };
  for (Connection &connection : view.connections)
    std::erase_if(connection.to, ends_here);
  std::erase_if(view.connections, [&](const Connection &connection) {
    return ends_here(connection.from) || connection.to.empty();
  });
  std::erase_if(view.nodes, [&](const Node &node) { return within(node.name, name); });
}

// The node goes with the nodes inside it, in their order, just before the other: the
// order a view lists its nodes in is the order it draws them and, where neither reads
// the other, cooks them, which a save keeps (V00).
void node_move(View &view, Arguments arguments, const Around &) {
  const std::string name(arguments[0]);
  const std::string before(arguments[1]);
  existing(view, name);
  existing(view, before);
  if (within(before, name))
    throw std::runtime_error(
        std::format("{} is {} or inside it, so it cannot go before it", before, name));
  const auto moving = std::ranges::stable_partition(
      view.nodes, [&](const Node &node) { return !within(node.name, name); });
  std::vector<Node> moved(std::make_move_iterator(moving.begin()),
                          std::make_move_iterator(moving.end()));
  view.nodes.erase(moving.begin(), moving.end());
  view.nodes.insert(std::ranges::find(view.nodes, before, &Node::name),
                    std::make_move_iterator(moved.begin()),
                    std::make_move_iterator(moved.end()));
}

void node_set(View &view, Arguments arguments, const Around &around) {
  Node &node = existing(view, arguments.front());
  Node changed = node;
  changed.where = around.where;
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
void connect_ports(View &view, Arguments arguments, const Around &around) {
  Connection connection{.name = std::string(arguments[0]),
                        .from = Edits::endpoint(arguments[1])};
  for (const std::string_view to : arguments.subspan(2))
    connection.to.push_back(Edits::endpoint(to));
  Edits::connect(view, std::move(connection), around.beside);
}

void disconnect(View &view, Arguments arguments, const Around &) {
  if (std::erase_if(view.connections, [&](const Connection &connection) {
        return connection.name == arguments.front();
      }) == 0)
    throw std::runtime_error(std::format("no connection is named {}", arguments.front()));
}

// A node inside a recipe a node uses keeps its params in that node, as the manifest
// writes them.
void param_set(View &view, Arguments arguments, const Around &around) {
  check_name("param", arguments[1]);
  check_value(arguments[1], arguments[2]);
  if (!node_named(view, arguments[0]) && user_of(view, arguments[0])) {
    auto [user, key] = user_param(view, arguments[0], arguments[1]);
    set_param(user.params, key, arguments[2]);
    user.where = around.where;
    return;
  }
  Node &node = existing(view, arguments[0]);
  set_param(node.params, arguments[1], arguments[2]);
  node.where = around.where;
}

void param_unset(View &view, Arguments arguments, const Around &around) {
  const auto unset = [&](std::vector<Param> &params,
                         std::string_view key,
                         std::string &by) {
    if (std::erase_if(params, [&](const Param &param) { return param.key == key; }) == 0)
      throw std::runtime_error(
          std::format("node {} sets no param {}", arguments[0], arguments[1]));
    by = around.where;
  };
  if (!node_named(view, arguments[0]) && user_of(view, arguments[0])) {
    auto [user, key] = user_param(view, arguments[0], arguments[1]);
    return unset(user.params, key, user.where);
  }
  Node &node = existing(view, arguments[0]);
  unset(node.params, arguments[1], node.where);
}

// The file arrives absolute, as the command port resolves a <file>.
void view_add(View &view, Arguments arguments, const Around &around) {
  Edits::child(view,
               Child{.name = std::string(arguments[0]),
                     .file = std::filesystem::path(arguments[1]),
                     .where = std::string(around.where)});
}

// The connections that take the view's window go first, as they would take it from
// nothing, so a view presented is no longer; nothing takes its place. A view beside it
// that takes it lets go too, as the views follow the host's edit.
void view_close(View &view, Arguments arguments, const Around &) {
  if (std::erase_if(view.children, [&](const Child &child) {
        return child.name == arguments.front();
      }) == 0)
    throw std::runtime_error(std::format(
        "this view hosts no view named {}; view list lists them", arguments.front()));
  std::erase_if(view.connections, [&](const Connection &connection) {
    return connection.from.view() && connection.from.node == arguments.front();
  });
}

// What a line that made an edit answers, so a person sees it took, where the log names
// only what fails (C09): from the view as it was and as it is, in the manifest's words,
// paths from where vulpen started, as every answer names them.
template <class Texts> std::string listed(const Texts &texts) {
  std::string text;
  for (const auto &each : texts)
    text.append(text.empty() ? "" : ", ").append(each);
  return text;
}

// The words a line named, as the node holds them now, each key once; one it holds no
// longer, cleared.
std::string named_words(const View &view, std::string_view name, Arguments words) {
  const Node &node = *std::ranges::find(view.nodes, name, &Node::name);
  std::vector<std::string> said;
  std::set<std::string_view> keys;
  for (const std::string_view word : words) {
    const std::string_view key = key_and_value(word).first;
    if (!keys.insert(key).second)
      continue;
    const std::size_t before = said.size();
    for (const Word &held : words_of(node))
      if (held.key == key)
        said.push_back(std::format("{} = {}", key, held.value));
    if (said.size() == before)
      said.push_back(std::format("{} cleared", key));
  }
  return listed(said);
}

std::string node_added(const View &, const View &after, Arguments arguments) {
  return arguments.size() == 1
             ? std::format("added node {}", arguments.front())
             : std::format("added node {}: {}",
                           arguments.front(),
                           named_words(after, arguments.front(), arguments.subspan(1)));
}

std::string node_removed(const View &, const View &, Arguments arguments) {
  return std::format("removed node {}, keeping its folder", arguments.front());
}

std::string node_moved(const View &, const View &, Arguments arguments) {
  return std::format("moved node {} before {}", arguments[0], arguments[1]);
}

std::string node_was_set(const View &, const View &after, Arguments arguments) {
  return std::format("set node {}: {}",
                     arguments.front(),
                     named_words(after, arguments.front(), arguments.subspan(1)));
}

std::string connected(const View &, const View &, Arguments arguments) {
  return std::format("connected {} to {} as {}",
                     arguments[1],
                     listed(arguments.subspan(2)),
                     arguments[0]);
}

std::string disconnected(const View &before, const View &, Arguments arguments) {
  const Connection &gone =
      *std::ranges::find(before.connections, arguments.front(), &Connection::name);
  return std::format("disconnected {}, from {} to {}",
                     gone.name,
                     gone.from.text(),
                     listed(gone.to | std::views::transform(&Endpoint::text)));
}

std::string param_was_set(const View &, const View &, Arguments arguments) {
  return std::format("set {}'s {} to {}", arguments[0], arguments[1], arguments[2]);
}

std::string param_was_unset(const View &, const View &, Arguments arguments) {
  return std::format("unset {}'s {}", arguments[0], arguments[1]);
}

std::string view_added(const View &, const View &, Arguments arguments) {
  return std::format("hosts view {}, from {}",
                     arguments[0],
                     std::filesystem::path(arguments[1])
                         .lexically_proximate(std::filesystem::current_path())
                         .generic_string());
}

std::string view_closed(const View &, const View &, Arguments arguments) {
  return std::format("closed view {}, its files kept", arguments.front());
}

std::string view_put_back(const View &, const View &, Arguments arguments) {
  return std::format("closed view {}, its files put back as its last view save left them",
                     arguments.front());
}

struct Edit {
  std::string_view usage;
  std::string_view help;
  void (*change)(View &view, Arguments arguments, const Around &around);
  std::string (*said)(const View &before, const View &after, Arguments arguments);
};

// Each edit's usage and help (RV04), and what it changes.
constexpr std::array edits{
    Edit{"node add <name> [<word=value>...]",
         "adds a node, given the manifest's node words; its files are what its folder "
         "holds",
         node_add,
         node_added},
    Edit{"node remove keep <node>",
         "removes a node with the nodes inside it, the connections they write, and them "
         "from the connections they read, keeping its folder",
         node_remove,
         node_removed},
    Edit{"node move <node> <node>",
         "lists a node, with the nodes inside it, just before another, so it draws under "
         "that one",
         node_move,
         node_moved},
    Edit{"node set <node> <word=value>...",
         "gives a node the words named, clearing those given empty; param and image "
         "words replace them all",
         node_set,
         node_was_set},
    Edit{"connect <name> <port> <port>...",
         "joins the port that writes a buffer to the ports that read it",
         connect_ports,
         connected},
    Edit{"disconnect <connection>", "removes a connection", disconnect, disconnected},
    Edit{"param set <node> <key> <value>",
         "sets a param of a node",
         param_set,
         param_was_set},
    Edit{"param unset <node> <key>",
         "removes a param of a node",
         param_unset,
         param_was_unset},
    Edit{"view add <name> <file>",
         "hosts the view a view.vlp holds, or an empty one that view save writes; a line "
         "`<name>: <command>` addresses it",
         view_add,
         view_added},
    Edit{"view close <view>",
         "stops hosting a view, letting go of the connections that take its window; its "
         "files stay, unless a project's differ from its last view save, which refuses",
         view_close,
         view_closed},
    Edit{"view close force <view>",
         "stops hosting a view as view close does, a project's files put back first as "
         "its last view save, or its load, left them",
         view_close,
         view_put_back},
};

constexpr std::string_view close_force = "view close force <view>";
// The files a refusal names at most, the rest counted.
constexpr std::size_t named_most = 6;

} // namespace

Edits::Edits(Commands &commands, ViewLookup &views) : _port(commands), _views(views) {
  for (const Edit &edit : edits)
    _commands.push_back(commands.add(edit.usage, edit.help, *this, Primitive::edit));
}

void Edits::word(Node &node, std::string_view key, std::string_view value) {
  if (key == "file")
    no_file(node.name);
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
  } else if (key == "log") {
    if (!level_named(value))
      throw std::runtime_error(
          std::format("log = {} is not error, warn, info or debug", value));
    set_once(node.log, key, value);
  } else if (key == "invocations") {
    set_count(node.invocations, key, value, "on a node with no .comp");
  } else if (key == "vertex_count") {
    set_count(node.vertex_count, key, value, "on a node with no .vert");
  } else if (key == "instance_count") {
    set_instances(node.instance_count, value);
  } else if (key == "param") {
    add_param(node, value);
  } else if (key == "image") {
    add_image(node, value);
  } else {
    unknown_word(key);
  }
}

// The port follows the last dot, so a node inside others keeps its own: ui.panel.rects.
// A view's name and a colon, as a line addresses the view, names its window.
Endpoint Edits::endpoint(std::string_view text) {
  if (text.ends_with(':') && is_name(text.substr(0, text.size() - 1)))
    return {std::string(text.substr(0, text.size() - 1)), {}};
  const std::size_t dot = text.rfind('.');
  const std::string_view node = text.substr(0, dot);
  const std::string_view port =
      dot == std::string_view::npos ? std::string_view{} : text.substr(dot + 1);
  if (!is_path(node) || !is_name(port))
    throw std::runtime_error(
        std::format("{} is not node.port, or view: for a hosted view's window", text));
  return {std::string(node), std::string(port)};
}

// A node's folder is its name's path in the view's folder (RV08), beside the folders of
// the views it hosts and of its contracts and shared code, so none of them shares a name.
void Edits::add(View &view, Node node) {
  check_path("node", node.name);
  if (node.name == contracts_folder || node.name == shared_folder)
    throw std::runtime_error(std::format("{}/ holds what the view's nodes share (RV05, "
                                         "RV09), so no node is named {}",
                                         node.name,
                                         node.name));
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

void Edits::connect(View &view,
                    Connection connection,
                    std::span<const std::string> beside) {
  check_path("connection", connection.name);
  if (std::ranges::find(view.connections, connection.name, &Connection::name) !=
      view.connections.end())
    throw std::runtime_error(
        std::format("two connections are named {}", connection.name));
  if (connection.from.node.empty() || connection.to.empty())
    throw std::runtime_error(
        std::format("connection {} needs a from and at least one to", connection.name));
  const Endpoint &from = connection.from;
  if (from.view() &&
      std::ranges::find(view.children, from.node, &Child::name) == view.children.end() &&
      std::ranges::find(beside, from.node) == beside.end())
    throw std::runtime_error(std::format(
        "connection {} is from {}, but this view hosts no view {}, nor is one beside it",
        connection.name,
        from.text(),
        from.node));
  if (!from.view() && !has_node(view, from.node))
    throw std::runtime_error(std::format(
        "connection {} is from {}, which is no node", connection.name, from.node));
  for (const Endpoint &to : connection.to)
    if (to.view())
      throw std::runtime_error(
          std::format("connection {} is to {}, but a view's window only leaves it, for "
                      "the view that hosts it",
                      connection.name,
                      to.text()));
    else if (!has_node(view, to.node))
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
// A hosted view's name is its own, so view close reaches the view that hosts it from
// wherever the line goes, as from inside the view it closes.
void Edits::command(Call &call) {
  std::string address(_port.addressed());
  for (std::size_t index = 0; index < edits.size(); ++index)
    if (call.is(_commands[index]) && edits[index].change == view_close)
      if (const std::optional<std::string> host =
              _views.host_of(call.arguments().front()))
        address = *host;
  const View *const found = _views.find(address);
  if (!found)
    throw std::runtime_error(std::format("no view is named {}", address));
  _port.changes(address);
  std::vector<std::string> beside;
  if (const std::optional<std::string> host = _views.host_of(address))
    for (const Child &child : _views.find(*host)->children)
      if (child.name != address)
        beside.push_back(child.name);
  const std::string current(_port.current());
  View edited = *found;
  std::string said;
  for (std::size_t index = 0; index < edits.size(); ++index)
    if (call.is(_commands[index])) {
      edits[index].change(edited, call.arguments(), {_port.where(), beside});
      if (edits[index].change == view_close)
        let_go(call.arguments().front(), edits[index].usage == close_force);
      said = edits[index].said(*found, edited, call.arguments());
    }
  _views.replace(address, std::move(edited));
  // Closing the view lines go to moves them on, which the answer names, as view switch
  // does.
  if (const std::string_view now = _port.current(); now != current)
    said += now.empty() ? std::string("; lines go to the view vulpen started with now")
                        : std::format("; lines go to {} now", now);
  call.reply(said);
}

// A project closes only as its last view save left it (1c): view close refuses while its
// files differ from that, naming them, so a close never loses work by a slip, and view
// close force puts them back first. Called once the close is known to be one, so a
// refused one puts nothing back.
void Edits::let_go(std::string_view view, bool force) {
  if (force)
    return _views.put_back(view);
  const std::vector<std::string> changed = _views.unkept(view);
  if (changed.empty())
    return;
  std::string named;
  for (const std::string &file :
       std::span(changed).first(std::min(changed.size(), named_most)))
    named.append(named.empty() ? "" : ", ").append(file);
  if (changed.size() > named_most)
    named += std::format(" and {} more", changed.size() - named_most);
  throw std::runtime_error(std::format("view close {} would lose what no view save kept: "
                                       "{}; view save keeps it, view close force puts "
                                       "the files back as last saved",
                                       view,
                                       named));
}

} // namespace VP
