#include "runtime/Operator.h"
#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::string_view manifest = "view.vlp";
constexpr std::string_view contracts = "contracts";
// How a node's file names a contract (RV05), so a drop knows which to copy.
constexpr std::string_view contract_include = "#include \"contracts/";
// The template's files, in the library part's template node, and the words in them that
// name the node: its C++ class, and its name. Each ends in .in, so it builds only once
// node new writes it.
constexpr std::array<std::string_view, 5> template_files{
    "Name.cpp", "Name.glsl", "Name.vert", "Name.frag", "Name.comp"};
constexpr std::string_view template_suffix = ".in";
constexpr std::string_view template_class = "Name";
constexpr std::string_view template_node = "name";
// What each kind of node starts with, by the template's files, and what it counts: one
// triangle, or one workgroup of the template's compute shader.
constexpr std::array<std::size_t, 4> draw_files{0, 1, 2, 3};
constexpr std::array<std::size_t, 2> dispatch_files{0, 4};
constexpr std::string_view draw_count = "vertex_count=3";
constexpr std::string_view dispatch_count = "invocations=64";
// Between a drop's recipe and the fingerprint of what it copied: probe@3f2a9c1e.
constexpr char fingerprint_mark = '@';
// FNV-1a over 64 bits: the same fingerprint on every machine and compiler (C01).
constexpr std::uint64_t fnv_offset = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;
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

// As text, since neither path need exist.
bool inside(const std::filesystem::path &path, const std::filesystem::path &folder) {
  const std::filesystem::path relative =
      path.lexically_normal().lexically_relative(folder.lexically_normal());
  return !relative.empty() && *relative.begin() != "..";
}

// Names of letters, digits, _ and -, joined by dots: the nodes a node is inside, then
// its own.
bool is_path(std::string_view text) {
  const auto name = [](std::string_view part) {
    return !part.empty() && std::ranges::all_of(part, [](char letter) {
      return (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
             (letter >= '0' && letter <= '9') || letter == '_' || letter == '-';
    });
  };
  for (std::size_t dot = text.find('.'); dot != std::string_view::npos;
       dot = text.find('.')) {
    if (!name(text.substr(0, dot)))
      return false;
    text.remove_prefix(dot + 1);
  }
  return name(text);
}

// ui.panel as the path ui/panel, as a node's folder is its name's path (RV08).
std::filesystem::path path_of(std::string_view name) {
  std::filesystem::path path;
  for (std::size_t dot = name.find('.'); dot != std::string_view::npos;
       dot = name.find('.')) {
    path /= name.substr(0, dot);
    name.remove_prefix(dot + 1);
  }
  return path / name;
}

// A new node's own name, which names its C++ class too: a lowercase letter, then
// lowercase letters, digits and -.
bool node_name(std::string_view name) {
  const auto lower = [](char letter) { return letter >= 'a' && letter <= 'z'; };
  return !name.empty() && lower(name.front()) &&
         std::ranges::all_of(name, [&](char letter) {
           return lower(letter) || (letter >= '0' && letter <= '9') || letter == '-';
         });
}

// The C++ class a node's name gives: command-line gives CommandLine.
std::string class_of(std::string_view name) {
  std::string type;
  bool upper = true;
  for (const char letter : name) {
    if (letter == '-') {
      upper = true;
      continue;
    }
    type.push_back(upper && letter >= 'a' && letter <= 'z'
                       ? static_cast<char>(letter - 'a' + 'A')
                       : letter);
    upper = false;
  }
  return type;
}

bool word_letter(char letter) {
  return (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
         (letter >= '0' && letter <= '9') || letter == '_';
}

// Each whole word `from` in the text, whether code or a comment's, becomes `to`.
std::string replaced(std::string text, std::string_view from, std::string_view to) {
  for (std::size_t at = text.find(from); at != std::string::npos;
       at = text.find(from, at)) {
    const std::size_t end = at + from.size();
    if ((at != 0 && word_letter(text[at - 1])) ||
        (end != text.size() && word_letter(text[end]))) {
      at = end;
      continue;
    }
    text.replace(at, from.size(), to);
    at += to.size();
  }
  return text;
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
  if (name == root)
    return std::string();
  if (name.size() > root.size() && name.starts_with(root) && name[root.size()] == '.')
    return std::string(name.substr(root.size() + 1));
  return std::nullopt;
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
  const std::string length = std::format("{}:", piece.size());
  for (const std::string_view text : {std::string_view(length), piece})
    for (const char letter : text) {
      hash ^= static_cast<unsigned char>(letter);
      hash *= fnv_prime;
    }
}

// What a copy is made of, but its params and log, which are its user's to set (V02): its
// nodes' names, operators, counts and files, and the connections between them.
std::string fingerprint(VP::FilePort &files, const Copy &copy) {
  std::vector<const Inner *> nodes;
  for (const Inner &inner : copy.nodes)
    nodes.push_back(&inner);
  std::ranges::sort(nodes, {}, &Inner::name);
  std::uint64_t hash = fnv_offset;
  for (const Inner *const inner : nodes) {
    const VP::Node &node = inner->node;
    hash_in(hash,
            std::format("node {} {} {} {} {}",
                        inner->name,
                        node.operator_name,
                        node.invocations,
                        node.vertex_count,
                        node.instance_count));
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

// A node's words as node add or node set takes them, but its files, which are what its
// folder holds. Set gives each word the node lacks empty, so it clears it.
std::string
words_of(const VP::Node &node, const std::vector<VP::Param> &params, bool set) {
  std::string words;
  const auto word = [&](std::string_view key, const std::string &value) {
    if (!value.empty())
      words += sendable(key, value);
    else if (set)
      words += std::format(" {}=", key);
  };
  const auto count = [](std::uint32_t value) {
    return value == 0 ? std::string() : std::to_string(value);
  };
  word("operator", node.operator_name);
  word("invocations", count(node.invocations));
  word("vertex_count", count(node.vertex_count));
  word("instance_count", count(node.instance_count));
  for (const VP::Param &param : params)
    words += sendable("param", std::format("{}={}", param.key, param.value));
  if (set && params.empty())
    words += " param=";
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

// Works on recipe and view folders through the file port: lists recipes, starts nodes
// from the template, drops and syncs recipes, and makes and loads views. A drop copies
// files, and then adds nodes, so the view owns its copy from the first edit on (V03).
class Library final : public VP::Operator {
  void bind(VP::Bind &node) override {
    // A part's folder is <library>/<kind>/<name>.
    _library = std::filesystem::path(node.folder()).parent_path().parent_path();
    for (std::size_t index = 0; index < template_files.size(); ++index)
      _templates[index] =
          node.file(std::format("template/{}{}", template_files[index], template_suffix));
    _list = node.command("recipe list", "lists the library's recipes, by kind");
    _draw = node.command("node new draw <name>",
                         "adds a draw node, and writes its first files from the template "
                         "into its folder: its C++, its pass block and its two shaders");
    _dispatch = node.command("node new dispatch <name>",
                             "adds a dispatch node, and writes its first files from the "
                             "template into its folder: its C++ and its compute shader");
    _drop = node.command("recipe drop <recipe> <name>",
                         "copies a library recipe into the view as a node of that name, "
                         "with the nodes inside it and the contracts they include");
    _sync =
        node.command("recipe sync <node>",
                     "brings a dropped recipe up to the library's, while what it copied "
                     "is unchanged; its params and connections stay");
    _new = node.command("view new <file>",
                        "hosts a new, empty view in a folder, and saves its view.vlp");
    _load = node.command("view load <file>", "hosts the view in a folder's view.vlp");
  }

  void command(VP::Call &call) override {
    if (call.is(_list))
      list(call);
    else if (call.is(_draw))
      start(call, draw_files, draw_count);
    else if (call.is(_dispatch))
      start(call, dispatch_files, dispatch_count);
    else if (call.is(_drop))
      drop(call);
    else if (call.is(_sync))
      sync(call);
    else if (call.is(_new) || call.is(_load))
      host(call, call.is(_new));
  }

  // A recipe is a folder in a kind's folder; the contracts' folder holds only files.
  void list(VP::Call &call) const {
    for (const std::string &kind : folders(call.files(), _library))
      for (std::string recipe : folders(call.files(), _library / kind)) {
        recipe.pop_back();
        call.reply(kind + recipe);
      }
  }

  // Where a recipe of the library is, by its name.
  std::filesystem::path find(VP::FilePort &files, std::string_view recipe) const {
    const std::string folder = std::string(recipe) + '/';
    for (const std::string &kind : folders(files, _library))
      if (holds(files, _library / kind, folder))
        return _library / kind / recipe;
    throw std::runtime_error(
        std::format("no recipe {} in the library; recipe list lists them", recipe));
  }

  // The folder of the view a command addresses, where its nodes' folders are (RV08). The
  // library's recipes are its own (V11), so a view of the library is never one.
  std::filesystem::path view_folder(VP::Call &call) const {
    const std::filesystem::path folder = call.view().file.parent_path();
    if (inside(folder, _library))
      throw std::runtime_error(std::format(
          "{} is in the library, whose recipes are its own; view new or view load hosts "
          "a view to work on",
          call.view().file.string()));
    return folder;
  }

  // A new node's name: free in the view, inside a node the view has when it holds a dot,
  // and naming a folder that is not there yet, since the node's folder is its own.
  static void
  check_new(VP::Call &call, const std::filesystem::path &view, std::string_view name) {
    if (!is_path(name))
      throw std::runtime_error(std::format("{} is no node's name: names of letters, "
                                           "digits, _ and -, joined by dots",
                                           name));
    const VP::View &graph = call.view();
    if (std::ranges::find(graph.nodes, name, &VP::Node::name) != graph.nodes.end())
      throw std::runtime_error(std::format("the view has a node {} already", name));
    const std::size_t dot = name.rfind('.');
    if (dot != std::string_view::npos &&
        std::ranges::find(graph.nodes, name.substr(0, dot), &VP::Node::name) ==
            graph.nodes.end())
      throw std::runtime_error(
          std::format("{} is inside {}, which is no node", name, name.substr(0, dot)));
    const std::filesystem::path folder = view / path_of(name);
    if (holds(call.files(), folder.parent_path(), folder.filename().string() + '/'))
      throw std::runtime_error(std::format(
          "{} is a folder already, and a new node's folder is its own", folder.string()));
  }

  // A node's first files from the template, in the folder its name names, and the node.
  // They build at once and do nothing yet; the lines of the menu wait, commented.
  void start(VP::Call &call,
             std::span<const std::size_t> files,
             std::string_view count) const {
    const std::string_view name = call.arguments().front();
    const std::string_view own = name.substr(name.rfind('.') + 1);
    if (!node_name(own))
      throw std::runtime_error(std::format("{} is no name for a new node: a lowercase "
                                           "letter, then lowercase letters, digits and -",
                                           own));
    const std::filesystem::path view = view_folder(call);
    check_new(call, view, name);
    const std::filesystem::path folder = view / path_of(name);
    const std::string type = class_of(own);
    std::string wrote;
    for (const std::size_t file : files) {
      const std::string named =
          type + std::string(template_files[file].substr(template_class.size()));
      call.files().save(
          (folder / named).string(),
          replaced(replaced(std::string(call.files().text(_templates[file])),
                            template_class,
                            type),
                   template_node,
                   name));
      wrote.append(wrote.empty() ? "" : ", ").append(named);
    }
    call.commands().send(std::format("node add {} operator={} {}", name, type, count));
    call.reply(
        std::format("wrote {} in {}, and added node {}", wrote, folder.string(), name));
  }

  // A recipe as a drop copies it: its own node, named like its folder, and the nodes
  // inside it, with each recipe they use unfolded, as a view would run it (V11).
  Copy recipe(VP::FilePort &files, const std::string &recipe) const {
    const VP::View graph = files.manifest((find(files, recipe) / manifest).string());
    if (std::ranges::find(graph.nodes, recipe, &VP::Node::name) == graph.nodes.end())
      throw std::runtime_error(
          std::format("recipe {} has no node {}: a recipe is one node "
                      "named like its folder, and the nodes inside it",
                      recipe,
                      recipe));
    for (const VP::Node &node : graph.nodes)
      if (!inner_name(recipe, node.name))
        throw std::runtime_error(std::format(
            "recipe {} holds node {}, which is not inside its node {}: a recipe is one "
            "node, and the nodes inside it",
            recipe,
            node.name,
            recipe));
    return copy_of(graph, recipe);
  }

  // A view's copy of a drop, each node with its folder in the view.
  static Copy copy_in(const VP::View &view,
                      const std::filesystem::path &folder,
                      std::string_view root) {
    Copy copy = copy_of(view, root);
    for (Inner &inner : copy.nodes)
      inner.node.folder = folder / path_of(outer_name(root, inner.name));
    return copy;
  }

  // Each node's files into the folder its name names in the view, saved whole (RA04),
  // and each contract they include that the view lacks, from the library (RV05).
  void write(VP::FilePort &files,
             const Copy &copy,
             const std::filesystem::path &view,
             std::string_view root) const {
    std::set<std::string> included;
    for (const Inner &inner : copy.nodes)
      for (const std::string &file : inner.node.files) {
        const std::string text = files.read((inner.node.folder / file).string());
        for (std::size_t at = text.find(contract_include); at != std::string::npos;
             at = text.find(contract_include, at + 1)) {
          const std::size_t start = at + contract_include.size();
          included.insert(text.substr(start, text.find('"', start) - start));
        }
        files.save((view / path_of(outer_name(root, inner.name)) / file).string(), text);
      }
    for (const std::string &contract : included)
      if (!holds(files, view / contracts, contract))
        files.save((view / contracts / contract).string(),
                   files.read((_library / contracts / contract).string()));
  }

  // The edits that add a copy's nodes under root, its own node naming the recipe it came
  // from, and join the connections between them.
  static std::vector<std::string>
  adds(const Copy &copy, std::string_view root, std::string_view recipe) {
    std::vector<std::string> lines;
    for (const Inner &inner : copy.nodes)
      lines.push_back(std::format("node add {}{}{}",
                                  outer_name(root, inner.name),
                                  inner.name.empty() ? sendable("recipe", recipe) : "",
                                  words_of(inner.node, inner.node.params, false)));
    for (const VP::Connection &connection : copy.connections)
      lines.push_back(connect_line(root, connection));
    return lines;
  }

  // A copy of a library recipe, as the node named and the nodes inside it (V03). The
  // node keeps its recipe and the fingerprint of what it copied, so a sync can tell a
  // copy left as it was from one someone made their own. Its edits are known before a
  // file is written, so one that cannot be sent leaves the view as it was.
  void drop(VP::Call &call) const {
    const std::string from(call.arguments()[0]);
    const std::string name(call.arguments()[1]);
    VP::FilePort &files = call.files();
    const std::filesystem::path view = view_folder(call);
    check_new(call, view, name);
    const Copy copy = recipe(files, from);
    const std::string print = fingerprint(files, copy);
    const std::vector<std::string> lines =
        adds(copy, name, from + fingerprint_mark + print);
    write(files, copy, view, name);
    for (const std::string &line : lines)
      call.commands().send(line);
    call.reply(std::format("copied {} into {}, as node {} of recipe {}{}{}",
                           from,
                           (view / path_of(name)).string(),
                           name,
                           from,
                           fingerprint_mark,
                           print));
  }

  // A connection from the rest of the view is the view's own, which a sync keeps, so
  // the library's version must still hold each node one reaches.
  static void check_outside(const VP::View &view,
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
              std::format("connection {} reaches {}, which the library's {} no longer "
                          "holds; drop it beside {} to take the new one",
                          connection.name,
                          end->node,
                          recipe,
                          root));
    }
  }

  // The params the view gives the copy that the library's version no longer has, which
  // a sync lets go, for its answer to name. The view's value for each param the library
  // still has stays.
  static std::string let_go(std::string_view root, const Copy &mine, const Copy &fresh) {
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

  // The files the copy holds that the library's version does not, gone, and the
  // library's written over the copy's. Deepest first, so a folder left empty goes too.
  void rewrite(VP::FilePort &files,
               const Copy &mine,
               const Copy &fresh,
               const std::filesystem::path &view,
               std::string_view root) const {
    std::vector<const Inner *> old;
    for (const Inner &inner : mine.nodes)
      old.push_back(&inner);
    std::ranges::sort(old, std::greater{}, &Inner::name);
    for (const Inner *const inner : old) {
      const Inner *const now = fresh.find(inner->name);
      for (const std::string &file : inner->node.files)
        if (!now || std::ranges::find(now->node.files, file) == now->node.files.end())
          files.remove((inner->node.folder / file).string());
    }
    write(files, fresh, view, root);
  }

  // The recipe a dropped node came from, and the fingerprint of what it copied.
  static std::pair<std::string, std::string> origin(const VP::View &view,
                                                    std::string_view root) {
    const auto node = std::ranges::find(view.nodes, root, &VP::Node::name);
    if (node == view.nodes.end())
      throw std::runtime_error(std::format("no node is named {}", root));
    const std::size_t mark = node->recipe.find(fingerprint_mark);
    if (mark == std::string::npos)
      throw std::runtime_error(std::format(
          "node {} was not dropped: it names no recipe and fingerprint to sync with",
          root));
    return {node->recipe.substr(0, mark), node->recipe.substr(mark + 1)};
  }

  // A drop brought up to the library's version of its recipe, while what it copied is as
  // it was. Its params keep the values the view set, and its connections to the rest of
  // the view stay; a param the library let go goes too, and the answer names it. A
  // change made inside the copy is the view's own, which a sync would lose, so it
  // refuses.
  void sync(VP::Call &call) const {
    const std::string root(call.arguments().front());
    const VP::View view = call.view();
    const auto [from, print] = origin(view, root);
    VP::FilePort &files = call.files();
    const std::filesystem::path folder = view_folder(call);
    const Copy mine = copy_in(view, folder, root);
    if (fingerprint(files, mine) != print)
      throw std::runtime_error(
          std::format("node {} changed since it was dropped from {}, and a sync would "
                      "lose that; drop {} beside it to take the library's, and make the "
                      "change there again",
                      root,
                      from,
                      from));
    const Copy fresh = recipe(files, from);
    const std::string now = fingerprint(files, fresh);
    if (now == print)
      return call.reply(std::format("node {} is as the library's {} is", root, from));
    check_outside(view, root, fresh, from);
    const std::vector<std::string> lines =
        edits(root, mine, fresh, from + fingerprint_mark + now);
    const std::string gone = let_go(root, mine, fresh);
    rewrite(files, mine, fresh, folder, root);
    for (const std::string &line : lines)
      call.commands().send(line);
    call.reply(std::format("synced node {} with the library's {}: {} is now {}{}{}",
                           root,
                           from,
                           print,
                           now,
                           gone.empty() ? "" : "; the library let go of ",
                           gone));
  }

  // The graph's side of a sync, as the edits a person would type: the copy's own
  // connections out, the nodes the library lost out, deepest first, the rest given the
  // library's words, the new ones in, and the library's connections back.
  static std::vector<std::string> edits(std::string_view root,
                                        const Copy &mine,
                                        const Copy &fresh,
                                        std::string_view recipe) {
    std::vector<std::string> lines;
    for (const VP::Connection &connection : mine.connections)
      lines.push_back(std::format("disconnect {}", outer_name(root, connection.name)));
    std::vector<const Inner *> gone;
    for (const Inner &inner : mine.nodes)
      if (!fresh.find(inner.name))
        gone.push_back(&inner);
    std::ranges::sort(gone, std::greater{}, &Inner::name);
    for (const Inner *const inner : gone)
      lines.push_back(std::format("node remove {}", outer_name(root, inner->name)));
    for (const Inner &inner : fresh.nodes) {
      const Inner *const before = mine.find(inner.name);
      lines.push_back(std::format(
          "node {} {}{}{}",
          before ? "set" : "add",
          outer_name(root, inner.name),
          inner.name.empty() ? sendable("recipe", recipe) : "",
          words_of(inner.node,
                   before ? kept_params(inner.node, before->node) : inner.node.params,
                   before != nullptr)));
    }
    for (const VP::Connection &connection : fresh.connections)
      lines.push_back(connect_line(root, connection));
    return lines;
  }

  // A view is named for its folder, which the build tree mirrors.
  static void host(VP::Call &call, bool fresh) {
    std::filesystem::path folder(call.arguments().front());
    if (folder.filename().empty())
      folder = folder.parent_path();
    const bool exists = holds(call.files(), folder, manifest);
    if (fresh && exists)
      throw std::runtime_error(
          std::format("{} holds a view already; view load hosts it", folder.string()));
    if (!fresh && !exists)
      throw std::runtime_error(std::format(
          "{} holds no {}; view new starts a view there", folder.string(), manifest));
    const std::string name = folder.filename().string();
    call.commands().send(
        std::format(": child add {} {}", name, (folder / manifest).string()));
    if (fresh)
      call.commands().send(std::format("{}: view save", name));
  }

  std::filesystem::path _library;
  std::array<VP::File, template_files.size()> _templates;
  VP::Command _list;
  VP::Command _draw;
  VP::Command _dispatch;
  VP::Command _drop;
  VP::Command _sync;
  VP::Command _new;
  VP::Command _load;
};

} // namespace

VP_OPERATORS(registry) {
  registry.add<Library>("Library");
}
