#pragma once

// What the library part copies, kept apart from the commands that copy it, so neither
// file grows past what one reads whole (CPP12): the folders recipes are in, a recipe or a
// view's node as a drop copies it, what its files include, and its fingerprint.
#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Hash.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// What a drop copies besides the nodes' files: the contracts and the shared code they
// include, at the same path from the view's top (RV05, RV09). Vulpen's library keeps its
// shared code in parts/, beside the parts that include it, and lists it as no recipe; a
// group of a person's recipes keeps both at its top, as a view does.
constexpr std::string_view parts = "parts";
constexpr std::string_view components = "components";
// The recipes' folder holds a folder a group of recipes: vulpen's own, in its library by
// kind (RV06), beside its apps, and a person's, as custom/, where a recipe made goes
// unless a path names another group, a folder a recipe.
constexpr std::string_view vulpen_group = "vulpen";
constexpr std::string_view custom_group = "custom";
constexpr std::string_view library_folder = "library";
constexpr std::string_view include_start = "#include \"";
// Between a drop's recipe and the fingerprint of what it copied: probe@3f2a9c1e.
constexpr char fingerprint_mark = '@';
constexpr int half_bits = 32; // a fingerprint shows the hash folded to 32 bits

// The folders in a folder, each named with the / that marks it.
std::vector<std::string> folders(VP::FilePort &files, const std::filesystem::path &in) {
  std::vector<std::string> found;
  for (std::string &name : files.list(in.string()))
    if (name.ends_with('/'))
      found.push_back(std::move(name));
  return found;
}

bool holds(VP::FilePort &files,
           const std::filesystem::path &folder,
           std::string_view name) {
  const std::vector<std::string> names = files.list(folder.string());
  return std::ranges::find(names, name) != names.end();
}

// Whether a folder of a kind's, as the file port names them, is the shared code's.
bool shared_code(std::string_view kind, std::string_view folder) {
  return kind.substr(0, kind.size() - 1) == parts && folder == VP::shared_folder;
}

// A recipe: its folder, the group it is in and its name, which is its folder's.
struct Found {
  std::filesystem::path folder;
  std::string group;
  std::string name;
};

// A path as an answer names it, from where vulpen started, as the prompt names a folder.
std::string shown(const std::filesystem::path &path) {
  return path.lexically_proximate(std::filesystem::current_path()).generic_string();
}

// As text, since neither path need exist.
bool inside(const std::filesystem::path &path, const std::filesystem::path &folder) {
  const std::filesystem::path relative =
      path.lexically_normal().lexically_relative(folder.lexically_normal());
  return !relative.empty() && *relative.begin() != "..";
}

// What a file includes that a drop copies along, each once, in the order found: a
// contract or shared code by its path from the view's top, and, in a file of those, a
// file beside it by its name, as a shared header includes its .cpp.
void includes_of(std::string_view text,
                 const std::filesystem::path &folder,
                 std::vector<std::filesystem::path> &found) {
  for (std::size_t at = text.find(include_start); at != std::string_view::npos;
       at = text.find(include_start, at + 1)) {
    const std::size_t start = at + include_start.size();
    const std::filesystem::path name(text.substr(start, text.find('"', start) - start));
    if (name.empty())
      continue;
    const std::filesystem::path top = *name.begin();
    std::filesystem::path file;
    if (top == VP::contracts_folder || top == VP::shared_folder)
      file = name;
    else if (!folder.empty() && name == top)
      file = folder / name;
    if (!file.empty() && std::ranges::find(found, file) == found.end())
      found.push_back(std::move(file));
  }
}

// A node of a recipe, or of a view's copy of one, by its name inside it: empty for the
// recipe's own node. Its folder is where its files are.
struct Inner {
  std::string name;
  VP::Node node;
};

// A recipe as a drop copies it, or a view's copy of one: its nodes, each before the
// nodes inside it, and the connections between them, named inside it.
struct Copy {
  std::vector<Inner> nodes;
  std::vector<VP::Connection> connections;

  const Inner *find(std::string_view name) const {
    const auto found = std::ranges::find(nodes, name, &Inner::name);
    return found == nodes.end() ? nullptr : &*found;
  }
};

// A node's name inside the one named root; none for a node outside it.
std::optional<std::string> inner_name(std::string_view root, std::string_view name) {
  if (!VP::within(name, root))
    return std::nullopt;
  return name == root ? std::string() : std::string(name.substr(root.size() + 1));
}

std::string outer_name(std::string_view root, std::string_view inner) {
  return inner.empty() ? std::string(root) : std::format("{}.{}", root, inner);
}

// What a graph holds of the node named root and the nodes inside it, each after the
// node it is inside. A connection inside it keeps a name inside it, as a drop gives it.
Copy copy_of(const VP::View &graph, std::string_view root) {
  Copy copy;
  for (const VP::Node &node : graph.nodes)
    if (std::optional<std::string> name = inner_name(root, node.name))
      copy.nodes.push_back({std::move(*name), node});
  std::ranges::stable_sort(copy.nodes, {}, [](const Inner &inner) {
    return inner.name.empty() ? 0 : 1 + std::ranges::count(inner.name, '.');
  });
  const auto within = [&](const VP::Endpoint &end) {
    return inner_name(root, end.node).has_value();
  };
  for (VP::Connection connection : graph.connections) {
    if (!within(connection.from) || !std::ranges::all_of(connection.to, within))
      continue;
    if (std::optional<std::string> name = inner_name(root, connection.name))
      connection.name = std::move(*name);
    connection.from.node = *inner_name(root, connection.from.node);
    for (VP::Endpoint &to : connection.to)
      to.node = *inner_name(root, to.node);
    copy.connections.push_back(std::move(connection));
  }
  return copy;
}

// Each piece by its length and its bytes, so no two lists of pieces hash alike.
void hash_in(std::uint64_t &hash, std::string_view piece) {
  hash = hashed(hashed(hash, std::format("{}:", piece.size())), piece);
}

// What a copy is made of, but its params and log, which are its user's to set (V02): its
// nodes' names, operators, counts, image formats and files, and the connections between
// them.
std::string fingerprint(VP::FilePort &files, const Copy &copy) {
  std::vector<const Inner *> nodes;
  for (const Inner &inner : copy.nodes)
    nodes.push_back(&inner);
  std::ranges::sort(nodes, {}, &Inner::name);
  std::uint64_t hash = fnv_offset;
  for (const Inner *const inner : nodes) {
    const VP::Node &node = inner->node;
    // No instance_count hashes as 0, as it did while the word took only a number, so a
    // drop made before keeps its fingerprint.
    hash_in(hash,
            std::format("node {} {} {} {} {}",
                        inner->name,
                        node.operator_name,
                        node.invocations,
                        node.vertex_count,
                        node.instance_count.empty() ? "0" : node.instance_count));
    // Hashed only when given, so a drop made before the word keeps its fingerprint.
    for (const VP::ImagePort &image : node.images)
      hash_in(hash, std::format("image {}={}", image.port, image.format));
    for (const std::string &file : node.files) {
      hash_in(hash, file);
      hash_in(hash, files.read((node.folder / file).string()));
    }
  }
  std::vector<const VP::Connection *> connections;
  for (const VP::Connection &connection : copy.connections)
    connections.push_back(&connection);
  std::ranges::sort(connections, {}, &VP::Connection::name);
  for (const VP::Connection *const connection : connections) {
    hash_in(hash, connection->name);
    hash_in(hash, connection->from.node + '.' + connection->from.port);
    for (const VP::Endpoint &to : connection->to)
      hash_in(hash, to.node + '.' + to.port);
  }
  return std::format("{:08x}", static_cast<std::uint32_t>(hash ^ (hash >> half_bits)));
}

// A command's words carry no blanks, so a value holding one cannot be sent.
std::string sendable(std::string_view key, std::string_view value) {
  if (value.find_first_of(" \t") != std::string_view::npos)
    throw std::runtime_error(std::format(
        "{} {} holds a blank, which a command's word cannot carry", key, value));
  return std::format(" {}={}", key, value);
}

// A node's words as node add or node set takes them, with these params, but its recipe,
// which a drop and a sync name themselves, and its log, which is the view's to set (V02).
// Set gives each word the node lacks empty, so it clears it.
std::string words_of(VP::Node node, std::vector<VP::Param> params, bool set) {
  node.params = std::move(params);
  const std::vector<VP::Word> given = VP::words_of(node);
  std::string words;
  for (const std::string_view key : VP::node_words) {
    if (key == "recipe" || key == "log")
      continue;
    bool any = false;
    for (const VP::Word &word : given)
      if (word.key == key) {
        words += sendable(key, word.value);
        any = true;
      }
    if (set && !any)
      words += std::format(" {}=", key);
  }
  return words;
}

std::string connect_line(std::string_view root, const VP::Connection &connection) {
  std::string line = std::format("connect {} {}.{}",
                                 outer_name(root, connection.name),
                                 outer_name(root, connection.from.node),
                                 connection.from.port);
  for (const VP::Endpoint &to : connection.to)
    line += std::format(" {}.{}", outer_name(root, to.node), to.port);
  return line;
}

// The params a sync gives a node: the library's, with the values the view set kept.
std::vector<VP::Param> kept_params(const VP::Node &fresh, const VP::Node &mine) {
  std::vector<VP::Param> params = fresh.params;
  for (VP::Param &param : params)
    if (const auto set = std::ranges::find(mine.params, param.key, &VP::Param::key);
        set != mine.params.end())
      param.value = set->value;
  return params;
}

// A connection from the rest of the view is the view's own, which a sync keeps, so
// the recipe's version must still hold each node one reaches.
void check_outside(const VP::View &view,
                   std::string_view root,
                   const Copy &fresh,
                   std::string_view recipe) {
  for (const VP::Connection &connection : view.connections) {
    std::vector<const VP::Endpoint *> ends{&connection.from};
    for (const VP::Endpoint &to : connection.to)
      ends.push_back(&to);
    const bool outside = std::ranges::any_of(
        ends, [&](const VP::Endpoint *end) { return !inner_name(root, end->node); });
    for (const VP::Endpoint *const end : ends)
      if (const std::optional<std::string> name = inner_name(root, end->node);
          outside && name && !fresh.find(*name))
        throw std::runtime_error(
            std::format("connection {} reaches {}, which recipe {} no longer holds; "
                        "drop it beside {} to take the new one",
                        connection.name,
                        end->node,
                        recipe,
                        root));
  }
}

// The params the view gives the copy that the recipe's version no longer has, which a
// sync lets go, for its answer to name. The view's value for each param the recipe
// still has stays.
std::string let_go(std::string_view root, const Copy &mine, const Copy &fresh) {
  std::string names;
  for (const Inner &inner : mine.nodes)
    for (const VP::Param &param : inner.node.params) {
      const Inner *const now = fresh.find(inner.name);
      if (!now || std::ranges::find(now->node.params, param.key, &VP::Param::key) ==
                      now->node.params.end())
        names += std::format("{}param {}={} of {}",
                             names.empty() ? "" : ", ",
                             param.key,
                             param.value,
                             outer_name(root, inner.name));
    }
  return names;
}

} // namespace
