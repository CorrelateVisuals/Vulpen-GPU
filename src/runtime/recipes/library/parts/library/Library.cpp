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
// include, at the same path from the view's top (RV05, RV09). The library keeps its
// shared code in parts/, beside the parts that include it, and lists it as no recipe.
constexpr std::string_view parts = "parts";
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
    // A part's folder is <recipes>/library/<kind>/<name>.
    _library = std::filesystem::path(node.folder()).parent_path().parent_path();
    _list = node.command("recipe list", "lists the library's recipes, by kind");
    _drop = node.command("recipe drop <recipe> <name>",
                         "copies a library recipe into the view as a node of that name, "
                         "with the nodes inside it and the contracts and shared code "
                         "they include; then saves the view");
    _sync = node.command("recipe sync <node>",
                         "brings a dropped recipe up to the library's, while what it "
                         "copied is unchanged; its params and connections stay; then "
                         "saves the view");
  }

  void command(VP::Call &call) override {
    if (call.is(_list))
      list(call);
    else if (call.is(_drop))
      drop(call);
    else if (call.is(_sync))
      sync(call);
  }

  // A recipe is a folder in a kind's folder, but the shared code's; the contracts' folder
  // holds only files.
  void list(VP::Call &call) const {
    for (const std::string &kind : folders(call.files(), _library))
      for (std::string recipe : folders(call.files(), _library / kind)) {
        recipe.pop_back();
        if (!shared_code(kind, recipe))
          call.reply(kind + recipe);
      }
  }

  // Where a recipe of the library is, by its name.
  std::filesystem::path find(VP::FilePort &files, std::string_view recipe) const {
    const std::string folder = std::string(recipe) + '/';
    for (const std::string &kind : folders(files, _library))
      if (!shared_code(kind, recipe) && holds(files, _library / kind, folder))
        return _library / kind / recipe;
    throw std::runtime_error(
        std::format("no recipe {} in the library; recipe list lists them", recipe));
  }

  // The folder of the view a command addresses, where its nodes' folders are (RV08). The
  // library's recipes and the apps beside it use recipes as they are (V11), so a view of
  // theirs is never one.
  std::filesystem::path view_folder(VP::Call &call) const {
    const std::filesystem::path folder = call.view().file.parent_path();
    if (inside(folder, _library.parent_path()))
      throw std::runtime_error(std::format(
          "{} is {}, which holds no nodes of yours; view new <name> starts a project to "
          "make them in",
          folder.lexically_normal()
              .lexically_relative(_library.parent_path().lexically_normal())
              .generic_string(),
          inside(folder, _library) ? "a recipe of the library" : "an app, vulpen's own"));
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

  // A command that writes a node's files saves the view too, so its manifest holds every
  // node whose folder is there: a view closed and loaded again finds them all (RV08).
  static void save_view(VP::Call &call) {
    call.commands().send("view save");
  }

  // A recipe as a drop copies it: its own node, named like its folder, and the nodes
  // inside it, with each recipe they use unfolded, as a view would run it (V11).
  Copy recipe(VP::FilePort &files, const std::string &recipe) const {
    const VP::View graph =
        files.manifest((find(files, recipe) / VP::manifest_file).string());
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
  // and each contract and shared file they include that the view lacks, from the library,
  // with what those include in turn (RV05, RV09). One the view has stays as it is.
  void write(VP::FilePort &files,
             const Copy &copy,
             const std::filesystem::path &view,
             std::string_view root) const {
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
      const std::filesystem::path from =
          *file.begin() == VP::shared_folder ? _library / parts / file : _library / file;
      const std::string text = files.read(from.string());
      includes_of(text, file.parent_path(), included);
      files.save((view / file).string(), text);
    }
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
    save_view(call);
    call.reply(
        std::format("copied {} into {}, as node {} of recipe {}{}{}, and saved the view",
                    from,
                    (view / VP::path_of(name)).string(),
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
    const Copy mine = copy_in(files, view, folder, root);
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
    save_view(call);
    call.reply(std::format("synced node {} with the library's {}: {} is now {}{}{}, and "
                           "saved the view",
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

  std::filesystem::path _library;
  VP::Command _list;
  VP::Command _drop;
  VP::Command _sync;
};

} // namespace

VP_OPERATOR(Library);
