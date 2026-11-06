#include "runtime/Operator.h"
#include "runtime/View.h"
#include "shared/Hash.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
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

// Works on recipe and view folders through the file port: lists recipes, and drops and
// syncs them. A drop copies files, and then adds nodes, so the view owns its copy from
// the first edit on (V03).
class Library final : public VP::Operator {
  void bind(VP::Bind &node) override {
    // A part's folder is <recipes>/vulpen/library/<kind>/<name>.
    _library = std::filesystem::path(node.folder()).parent_path().parent_path();
    _recipes = _library.parent_path().parent_path();
    _list = node.command("recipe list",
                         "lists the recipes, each by its path from the recipes' folder: "
                         "vulpen's library's by kind, then each other group's, as "
                         "custom's");
    _drop = node.command("recipe drop <recipe> <name>",
                         "copies a recipe, by its name, or its group, a slash and its "
                         "name, as custom/arcball, into the view as a node of that name, "
                         "with the nodes inside it and the contracts and shared code "
                         "they include, or by a path a node's folder from any view, with "
                         "its words, as copy brings one");
    _sync = node.command("recipe sync <node>",
                         "brings a dropped recipe up to its recipe as it is now, while "
                         "what it copied is unchanged; its params and connections stay");
    _make = node.command("recipe make <node> <name>",
                         "makes the node a recipe by that name, in custom/, or by a "
                         "group, a slash and the name, in that group, vulpen's "
                         "library's by its kind, the node becoming a drop of it: a part "
                         "of its files and words, with the contracts and shared code "
                         "they include, for a node with none inside it; else a "
                         "component of the recipes the nodes inside it were dropped "
                         "from, unchanged since, their params and connections as the "
                         "view gives them; made again from a drop of it, it is saved "
                         "where it is");
  }

  void command(VP::Call &call) override {
    if (call.is(_list))
      list(call);
    else if (call.is(_drop))
      drop(call);
    else if (call.is(_sync))
      sync(call);
    else if (call.is(_make))
      make(call);
  }

  // A group with none yet by its folder, as custom/ before a recipe is made, so the
  // terminal still offers it.
  void list(VP::Call &call) const {
    for (const std::string &group : groups(call.files())) {
      const std::vector<Found> recipes = recipes_in(call.files(), group);
      if (recipes.empty())
        call.reply(group + '/');
      for (const Found &recipe : recipes)
        call.reply(from_recipes(recipe.folder));
    }
  }

  // A path from the recipes' folder, as list names a recipe and make takes one.
  std::string from_recipes(const std::filesystem::path &path) const {
    return path.lexically_normal().lexically_relative(_recipes.lexically_normal())
        .generic_string();
  }

  // Vulpen's first, then each other folder of the recipes' by name.
  std::vector<std::string> groups(VP::FilePort &files) const {
    std::vector<std::string> found{std::string(vulpen_group)};
    for (std::string group : folders(files, _recipes)) {
      group.pop_back();
      if (group != vulpen_group && !group.starts_with('.'))
        found.push_back(std::move(group));
    }
    return found;
  }

  // Vulpen's, a folder in a kind's folder of its library, but the shared code's, as the
  // contracts' folder holds only files; another group's, each of its folders that holds a
  // manifest, but its contracts' and its shared code's.
  std::vector<Found> recipes_in(VP::FilePort &files, std::string_view group) const {
    std::vector<Found> found;
    const auto add = [&](const std::filesystem::path &in, std::string recipe) {
      recipe.pop_back();
      found.push_back({in / recipe, std::string(group), recipe});
    };
    if (group == vulpen_group) {
      for (const std::string &kind : folders(files, _library))
        for (std::string recipe : folders(files, _library / kind))
          if (!shared_code(kind, recipe.substr(0, recipe.size() - 1)))
            add(_library / kind.substr(0, kind.size() - 1), std::move(recipe));
      return found;
    }
    const std::filesystem::path top = _recipes / group;
    for (std::string recipe : folders(files, top))
      if (recipe != std::format("{}/", VP::contracts_folder) &&
          recipe != std::format("{}/", VP::shared_folder) &&
          holds(files, top / recipe, VP::manifest_file))
        add(top, std::move(recipe));
    return found;
  }

  // Where a recipe is, by its name, or its group, a slash and its name, as list names
  // none twice but a person may copy one by hand into two groups, which then needs its
  // group named.
  Found find(VP::FilePort &files, std::string_view word) const {
    const std::size_t slash = word.find('/');
    const std::string_view name =
        slash == std::string_view::npos ? word : word.substr(slash + 1);
    std::vector<Found> found;
    for (const std::string &group : groups(files))
      if (slash == std::string_view::npos || group == word.substr(0, slash))
        for (Found &recipe : recipes_in(files, group))
          if (recipe.name == name)
            found.push_back(std::move(recipe));
    if (found.empty())
      throw std::runtime_error(
          std::format("no recipe {}; recipe list lists them", word));
    if (found.size() > 1)
      throw std::runtime_error(std::format("{} is a recipe of {} and of {}: {}/{} or "
                                           "{}/{} names one",
                                           name,
                                           found[0].group,
                                           found[1].group,
                                           found[0].group,
                                           name,
                                           found[1].group,
                                           name));
    return std::move(found.front());
  }

  // Whether a word names a recipe by its group, a slash and its name, rather than a
  // path, as a group is a folder of the recipes'.
  bool grouped(VP::FilePort &files, std::string_view word) const {
    const std::size_t slash = word.find('/');
    if (slash == std::string_view::npos || slash != word.rfind('/') ||
        !VP::is_name(word.substr(slash + 1)))
      return false;
    const std::vector<std::string> named = groups(files);
    return std::ranges::find(named, word.substr(0, slash)) != named.end();
  }

  // Where the contracts and the shared code a group's recipes include are: vulpen's
  // library, or the group's own top, as a view's.
  std::filesystem::path top_of(std::string_view group) const {
    return group == vulpen_group ? _library : _recipes / group;
  }

  // The folder of the view a command addresses, where its nodes' folders are (RV08). The
  // recipes and vulpen's apps use recipes as they are (V11), so a view of theirs is never
  // one.
  std::filesystem::path view_folder(VP::Call &call) const {
    const std::filesystem::path folder = call.view().file.parent_path();
    if (inside(folder, _recipes))
      throw std::runtime_error(std::format(
          "{} is {}, which holds no nodes of yours; view new <name> starts a project to "
          "make them in",
          from_recipes(folder),
          inside(folder, _library) || !inside(folder, _library.parent_path())
              ? "a recipe"
              : "an app, vulpen's own"));
    return folder;
  }

  // A new node's name: free in the view, inside a node the view has when it holds a dot,
  // and naming a folder that is not there yet, since the node's folder is its own.
  static void check_new(VP::Call &call,
                        const std::filesystem::path &view,
                        std::string_view name) {
    if (!VP::is_path(name))
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
    const std::filesystem::path folder = view / VP::path_of(name);
    if (holds(call.files(), folder.parent_path(), folder.filename().string() + '/'))
      throw std::runtime_error(std::format(
          "{} is a folder already, and a new node's folder is its own", folder.string()));
  }

  // A recipe as a drop copies it: its own node, named like its folder, and the nodes
  // inside it, with each recipe they use unfolded, as a view would run it (V11).
  Copy recipe(VP::FilePort &files, const Found &found) const {
    const std::string &recipe = found.name;
    const VP::View graph = files.manifest((found.folder / VP::manifest_file).string());
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

  // A view's copy of a drop, each node with its folder in the view and the files that
  // folder holds now, as the manifest read anew lists them: a file line changes a folder
  // between the edits that list it again, so the view's own list may be behind.
  static Copy copy_in(VP::FilePort &files,
                      const VP::View &view,
                      const std::filesystem::path &folder,
                      std::string_view root) {
    const VP::View read = files.manifest(view.file.string());
    Copy copy = copy_of(view, root);
    for (Inner &inner : copy.nodes) {
      inner.node.folder = folder / VP::path_of(outer_name(root, inner.name));
      if (const auto now = std::ranges::find(read.nodes, inner.node.name, &VP::Node::name);
          now != read.nodes.end())
        inner.node.files = now->files;
    }
    return copy;
  }

  // Each node's files into the folder its name names in the view, saved whole (RA04),
  // and each contract and shared file they include that the view lacks, from the top
  // they came from, vulpen's library, a group's or a view's, with what those include in
  // turn (RV05, RV09). One the view has stays as it is.
  void write(VP::FilePort &files,
             const Copy &copy,
             const std::filesystem::path &view,
             std::string_view root,
             const std::filesystem::path &top) const {
    std::vector<std::filesystem::path> included;
    for (const Inner &inner : copy.nodes)
      for (const std::string &file : inner.node.files) {
        const std::string text = files.read((inner.node.folder / file).string());
        includes_of(text, {}, included);
        files.save((view / VP::path_of(outer_name(root, inner.name)) / file).string(),
                   text);
      }
    for (std::size_t next = 0; next < included.size(); ++next) {
      const std::filesystem::path file = included[next];
      if (holds(files, view / file.parent_path(), file.filename().string()))
        continue;
      const std::string text = files.read(included_from(files, top, file).string());
      includes_of(text, file.parent_path(), included);
      files.save((view / file).string(), text);
    }
  }

  // Where a contract or shared code a copy includes is read from: the top it came from,
  // or vulpen's library, whose contracts a group's recipe or a view may include too.
  std::filesystem::path included_from(VP::FilePort &files,
                                      const std::filesystem::path &top,
                                      const std::filesystem::path &file) const {
    const std::filesystem::path own = top / file;
    if (top != _library && holds(files, own.parent_path(), own.filename().string()))
      return own;
    return in_library(file);
  }

  // The edits that add a copy's nodes under root, its own node naming the recipe it came
  // from, and join the connections between them.
  static std::vector<std::string>
  adds(const Copy &copy, std::string_view root, std::string_view recipe) {
    std::vector<std::string> lines;
    for (const Inner &inner : copy.nodes)
      lines.push_back(std::format("node add {}{}{}",
                                  outer_name(root, inner.name),
                                  inner.name.empty() && !recipe.empty()
                                      ? sendable("recipe", recipe)
                                      : "",
                                  words_of(inner.node, inner.node.params, false)));
    for (const VP::Connection &connection : copy.connections)
      lines.push_back(connect_line(root, connection));
    return lines;
  }

  // What a drop copies, the recipe its own node names, the top its contracts and shared
  // code come from, and the words the answer names it by: a recipe by its name, its group
  // and its name, or its own folder, its node keeping the recipe and the fingerprint of
  // what it copied, so a sync can tell a copy left as it was from one someone made their
  // own; or a node of a view by its folder, as the nearest manifest at or above it names
  // it, with its words as the view gives them while vulpen hosts it, else as the manifest
  // does, and the recipe it names itself, if any.
  struct Source {
    Copy copy;
    std::string recipe;
    std::filesystem::path top;
    std::string named;
  };

  Source source_of(VP::Call &call, std::string_view word) const {
    VP::FilePort &files = call.files();
    if (word.find('/') == std::string_view::npos || grouped(files, word)) {
      const Found found = find(files, word);
      Copy copy = recipe(files, found);
      std::string print = fingerprint(files, copy);
      return {std::move(copy),
              std::format("{}{}{}", found.name, fingerprint_mark, print),
              top_of(found.group),
              std::format("{}/{}", found.group, found.name)};
    }
    std::filesystem::path folder =
        std::filesystem::path(call.resolved(word)).lexically_normal();
    if (folder.filename().empty())
      folder = folder.parent_path();
    std::filesystem::path at = folder;
    for (; !holds(files, at, VP::manifest_file); at = at.parent_path())
      if (at == at.parent_path())
        throw std::runtime_error(std::format(
            "{} is in no view: a drop copies a node's folder, or a recipe's", word));
    if (at == folder) {
      if (!inside(folder, _recipes))
        throw std::runtime_error(std::format(
            "{} is a view's own folder: a drop copies a node's, or a recipe's", word));
      return source_of(call, folder.filename().string());
    }
    std::string node;
    for (const std::filesystem::path &part : folder.lexically_relative(at))
      node += (node.empty() ? "" : ".") + part.string();
    const std::filesystem::path manifest = at / VP::manifest_file;
    const VP::View read = files.manifest(manifest.string());
    const VP::View *const hosted = call.view(at.filename().string());
    const bool runs =
        hosted && hosted->file.lexically_normal() == manifest.lexically_normal();
    const VP::View &view = runs ? *hosted : read;
    const auto own = std::ranges::find(view.nodes, node, &VP::Node::name);
    if (own == view.nodes.end())
      throw std::runtime_error(std::format(
          "{} is no node's folder: {} names no node {}", word, manifest.string(), node));
    Source source{.copy = copy_of(view, node),
                  .recipe = own->recipe,
                  .top = at,
                  .named = shown(folder)};
    for (Inner &inner : source.copy.nodes) {
      inner.node.folder = at / VP::path_of(outer_name(node, inner.name));
      const auto listed = std::ranges::find(read.nodes, inner.node.name, &VP::Node::name);
      if (listed != read.nodes.end())
        inner.node.files = listed->files;
    }
    return source;
  }

  // A copy of a recipe of the library, or of a view's node, as the node named and the
  // nodes inside it (V03). Its edits are known before a file is written, so one that
  // cannot be sent leaves the view as it was.
  void drop(VP::Call &call) const {
    const std::string name(call.arguments()[1]);
    VP::FilePort &files = call.files();
    const std::filesystem::path view = view_folder(call);
    check_new(call, view, name);
    const Source source = source_of(call, call.arguments()[0]);
    const std::vector<std::string> lines = adds(source.copy, name, source.recipe);
    write(files, source.copy, view, name, source.top);
    for (const std::string &line : lines)
      call.commands().send(line);
    call.reply(std::format("copied {} into {}, as node {}{}",
                           source.named,
                           shown(view / VP::path_of(name)),
                           name,
                           source.recipe.empty()
                               ? ""
                               : std::format(" of recipe {}", source.recipe)));
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

  // The files the copy holds that the recipe's version does not, gone, and the recipe's
  // written over the copy's, from the top its group keeps its contracts in. Deepest
  // first, so a folder left empty goes too.
  void rewrite(VP::FilePort &files,
               const Copy &mine,
               const Copy &fresh,
               const std::filesystem::path &view,
               std::string_view root,
               const std::filesystem::path &top) const {
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
    write(files, fresh, view, root, top);
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

  // A drop brought up to its recipe as it is now, while what it copied is as it was. Its
  // params keep the values the view set, and its connections to the rest of the view
  // stay; a param the recipe let go goes too, and the answer names it. A change made
  // inside the copy is the view's own, which a sync would lose, so it refuses.
  void sync(VP::Call &call) const {
    const std::string root(call.arguments().front());
    const VP::View view = call.view();
    const auto [from, print] = origin(view, root);
    VP::FilePort &files = call.files();
    const std::filesystem::path folder = view_folder(call);
    const Copy mine = copy_in(files, view, folder, root);
    if (fingerprint(files, mine) != print)
      throw std::runtime_error(
          std::format("node {} changed since it was dropped from {}, and a sync would "
                      "lose that; drop {} beside it to take the recipe's, and make the "
                      "change there again",
                      root,
                      from,
                      from));
    const Found found = find(files, from);
    const Copy fresh = recipe(files, found);
    const std::string now = fingerprint(files, fresh);
    if (now == print)
      return call.reply(std::format(
          "node {} is as recipe {}/{} is", root, found.group, found.name));
    check_outside(view, root, fresh, from);
    const std::vector<std::string> lines =
        edits(root, mine, fresh, from + fingerprint_mark + now);
    const std::string gone = let_go(root, mine, fresh);
    rewrite(files, mine, fresh, folder, root, top_of(found.group));
    for (const std::string &line : lines)
      call.commands().send(line);
    call.reply(std::format("synced node {} with recipe {}/{}: {} is now {}{}{}",
                           root,
                           found.group,
                           found.name,
                           print,
                           now,
                           gone.empty() ? "" : "; the recipe let go of ",
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
      lines.push_back(std::format("node remove keep {}", outer_name(root, inner->name)));
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

  // A drop turned around, so a node a person made by hand is dropped elsewhere as a
  // recipe (V02): a part holds one job's files and a component only uses recipes
  // (RV06), so a node with nodes inside it is made of the recipes they were dropped
  // from, and one inside it with code of its own is refused, naming the line that makes
  // it a recipe first. Everything is checked before a file is written, and the node
  // then names the recipe and the fingerprint of what it holds, as a drop of it would,
  // so recipe sync brings its later changes to the other drops of it. A name another
  // recipe has is refused, unless the node is a drop of that one: it is saved again
  // where it is, as a person makes a recipe they dropped their own.
  void make(VP::Call &call) const {
    const std::string root(call.arguments()[0]);
    VP::FilePort &files = call.files();
    const std::filesystem::path view = view_folder(call);
    const VP::View &graph = call.view();
    const auto node = std::ranges::find(graph.nodes, root, &VP::Node::name);
    if (node == graph.nodes.end())
      throw std::runtime_error(std::format("no node is named {}", root));
    const Copy copy = copy_in(files, graph, view, root);
    const bool part = copy.nodes.size() == 1;
    const Found target = target_of(files, call.arguments()[1], part, *node);
    const bool again = holds(files, target.folder, VP::manifest_file);
    // Read before the node set below, which replaces the view.
    const std::string made =
        std::format("{} recipe {} of node {}{}, which is a drop of it now",
                    again ? "saved" : "made",
                    from_recipes(target.folder),
                    root,
                    part ? "" : ", using " + uses_named(graph, root));
    if (part)
      make_part(files, copy, target, view);
    else
      make_component(files, graph, copy, target.folder, root, target.name);
    const std::string print = fingerprint(files, recipe(files, target));
    call.commands().send(std::format(
        "node set {} recipe={}{}{}", root, target.name, fingerprint_mark, print));
    call.reply(made);
  }

  // Where a recipe made goes: a name, in custom/; a group, a slash and a name, in that
  // group, vulpen's in its library's kind for what it is (RV06), as a path to that kind
  // from the recipes' folder does too. A name a recipe has already, wherever it is, only
  // for a drop of that recipe, which is saved again where it is.
  Found target_of(VP::FilePort &files,
                  std::string_view word,
                  bool part,
                  const VP::Node &node) const {
    const std::string kind(part ? parts : components);
    const std::string library = std::format("{}/{}/", vulpen_group, library_folder);
    const bool in_kind = word.starts_with(library);
    if (in_kind && !word.substr(library.size()).starts_with(kind + '/'))
      throw std::runtime_error(std::format("{} goes in {}{}/, as a {} (RV06)",
                                           word,
                                           library,
                                           kind,
                                           kind.substr(0, kind.size() - 1)));
    const std::string_view rest =
        in_kind ? word.substr(library.size() + kind.size() + 1) : word;
    const std::size_t slash = rest.find('/');
    const std::string group(in_kind                ? vulpen_group
                            : slash == rest.npos ? custom_group
                                                 : rest.substr(0, slash));
    const std::string name(in_kind || slash == rest.npos ? rest : rest.substr(slash + 1));
    if (!VP::is_name(group) || !VP::is_name(name))
      throw std::runtime_error(std::format(
          "{} is no recipe's name: letters, digits, _ and -, named for its job (RV07), "
          "which goes in custom/, or a group, a slash and such a name, as vulpen/<name> "
          "for vulpen's library",
          word));
    if (group != vulpen_group &&
        (name == VP::contracts_folder || name == VP::shared_folder))
      throw std::runtime_error(std::format(
          "{}/ is where {} keeps what its recipes share (RV05, RV09)", name, group));
    for (const std::string &other : groups(files))
      for (Found &found : recipes_in(files, other))
        if (found.name == name) {
          if (node.recipe.substr(0, node.recipe.find(fingerprint_mark)) != name)
            throw std::runtime_error(std::format(
                "recipe {} is there already: recipe drop {} <name> uses it, and a "
                "drop of it is made a recipe again where it is",
                from_recipes(found.folder),
                name));
          return std::move(found);
        }
    return {group == vulpen_group ? _library / kind / name : _recipes / group / name,
            group,
            name};
  }

  // Where a contract or shared code goes in vulpen's library, as a drop finds it (RV05,
  // RV09); a group's go at its top, as a view's.
  std::filesystem::path in_library(const std::filesystem::path &file) const {
    return *file.begin() == VP::shared_folder ? _library / parts / file : _library / file;
  }

  // The node's files, and the contracts and shared code they include that neither the
  // recipe's group nor vulpen's library holds, which go to the group's; one either holds
  // otherwise is refused, as their other recipes read it (V11). A recipe made again
  // loses the files the node no longer holds.
  void make_part(VP::FilePort &files,
                 const Copy &copy,
                 const Found &target,
                 const std::filesystem::path &view) const {
    const VP::Node &node = copy.nodes.front().node;
    std::vector<std::filesystem::path> included;
    for (const std::string &file : node.files)
      includes_of(files.read((node.folder / file).string()), {}, included);
    for (std::size_t next = 0; next < included.size(); ++next)
      includes_of(files.read((view / included[next]).string()),
                  included[next].parent_path(),
                  included);
    const std::filesystem::path top = top_of(target.group);
    std::vector<std::filesystem::path> brought;
    for (const std::filesystem::path &file : included) {
      bool held = false;
      for (const std::filesystem::path &there :
           {top == _library ? in_library(file) : top / file, in_library(file)})
        if (holds(files, there.parent_path(), there.filename().string())) {
          if (files.read(there.string()) != files.read((view / file).string()))
            throw std::runtime_error(std::format(
                "{} differs from {}, which recipes include: name it apart",
                shown(view / file),
                shown(there)));
          held = true;
        }
      if (!held)
        brought.push_back(file);
    }
    for (const std::string &file : files.list(target.folder.string()))
      if (!file.ends_with('/') && file != VP::manifest_file &&
          std::ranges::find(node.files, file) == node.files.end())
        files.remove((target.folder / file).string());
    for (const std::string &file : node.files)
      files.save((target.folder / file).string(),
                 files.read((node.folder / file).string()));
    for (const std::filesystem::path &file : brought)
      files.save((top == _library ? in_library(file) : top / file).string(),
                 files.read((view / file).string()));
    VP::Node own = node;
    own.name = target.name;
    own.recipe.clear();
    own.log.clear();
    own.files.clear();
    own.where.clear();
    files.save(VP::View{.name = target.name, .nodes = {std::move(own)}},
               (target.folder / VP::manifest_file).string());
  }

  // The nodes a drop brought inside the node, each the one a drop named, and so in a
  // component a use of its recipe; a node inside one of them came with it.
  static std::vector<const VP::Node *> uses_of(const VP::View &graph, std::string_view root) {
    std::vector<const VP::Node *> uses;
    for (const VP::Node &node : graph.nodes)
      if (node.name != root && VP::within(node.name, root) &&
          node.recipe.find(fingerprint_mark) != std::string::npos &&
          std::ranges::none_of(uses, [&](const VP::Node *use) {
            return VP::within(node.name, use->name);
          }))
        uses.push_back(&node);
    return uses;
  }

  static std::string uses_named(const VP::View &graph, std::string_view root) {
    std::string named;
    for (const VP::Node *const use : uses_of(graph, root))
      named.append(named.empty() ? "" : ", ")
          .append(use->recipe.substr(0, use->recipe.find(fingerprint_mark)));
    return named;
  }

  // A use of each recipe a node inside was dropped from, with the params the view gives
  // it and the nodes inside it that differ from the recipe's, by their paths in it, and
  // the connections between them but those inside one use, which its recipe brings.
  void make_component(VP::FilePort &files,
                      const VP::View &graph,
                      const Copy &copy,
                      const std::filesystem::path &folder,
                      std::string_view root,
                      std::string_view name) const {
    if (!copy.nodes.front().node.files.empty())
      throw std::runtime_error(std::format(
          "{} holds files of its own, and a component holds only the recipes it uses "
          "(RV06): move them into a node inside it",
          root));
    const std::vector<const VP::Node *> uses = uses_of(graph, root);
    for (const Inner &inner : copy.nodes) {
      const std::string full = outer_name(root, inner.name);
      if (!inner.name.empty() && std::ranges::none_of(uses, [&](const VP::Node *use) {
            return VP::within(full, use->name);
          }))
        throw std::runtime_error(std::format(
            "{} holds code of its own, and a component holds only the recipes it uses "
            "(RV06): recipe make {} <name> makes it one first",
            full,
            full));
    }
    VP::View made{.name = std::string(name), .nodes = {VP::Node{.name = std::string(name)}}};
    for (const VP::Node *const use : uses) {
      const std::string from = use->recipe.substr(0, use->recipe.find(fingerprint_mark));
      if (fingerprint(files, copy_in(files, graph, graph.file.parent_path(), use->name)) !=
          use->recipe.substr(use->recipe.find(fingerprint_mark) + 1))
        throw std::runtime_error(std::format(
            "{} changed since it was dropped from {}, and a component uses {} as the "
            "library has it: recipe make {} <name> makes the change a recipe of its own",
            use->name,
            from,
            from,
            use->name));
      made.nodes.push_back({.name = outer_name(name, *inner_name(root, use->name)),
                            .recipe = from,
                            .params = changed_params(files, graph, *use, from)});
    }
    for (VP::Connection connection : copy.connections) {
      if (std::ranges::any_of(uses, [&](const VP::Node *use) {
            return VP::within(outer_name(root, connection.name), use->name);
          }))
        continue;
      connection.name = outer_name(name, connection.name);
      connection.from.node = outer_name(name, connection.from.node);
      for (VP::Endpoint &to : connection.to)
        to.node = outer_name(name, to.node);
      made.connections.push_back(std::move(connection));
    }
    files.save(made, (folder / VP::manifest_file).string());
  }

  // The params a use gives its recipe: each the view sets on the use, or on a node
  // inside it, that the recipe's own node there lacks or sets otherwise, a node inside
  // by its path in the use.
  std::vector<VP::Param> changed_params(VP::FilePort &files,
                                        const VP::View &graph,
                                        const VP::Node &use,
                                        const std::string &from) const {
    const Copy fresh = recipe(files, find(files, from));
    std::vector<VP::Param> params;
    for (const VP::Node &node : graph.nodes) {
      const std::optional<std::string> inner = inner_name(use.name, node.name);
      if (!inner)
        continue;
      const Inner *const theirs = fresh.find(*inner);
      for (const VP::Param &param : node.params)
        if (const VP::Param *const set = theirs ? theirs->node.param(param.key) : nullptr;
            !set || set->value != param.value)
          params.push_back({inner->empty() ? param.key : *inner + '.' + param.key,
                            param.value});
    }
    return params;
  }

  std::filesystem::path _library; // vulpen's
  std::filesystem::path _recipes; // the recipes' folder, which holds each group's
  VP::Command _list;
  VP::Command _drop;
  VP::Command _sync;
  VP::Command _make;
};

} // namespace

VP_OPERATOR(Library);
