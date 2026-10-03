#include "runtime/Manifest.h"

#include "baseclasses/Platform.h"
#include "runtime/Edits.h"
#include "runtime/View.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace VP {

namespace {

constexpr std::uint32_t manifest_version = 1;
constexpr std::string_view blanks = " \t\r";
// The library's folders by the kind of recipe each holds (RV06), and the view it builds
// as, which src/runtime/cmake/nodes.cmake names the same.
constexpr std::array<std::string_view, 3> recipe_kinds{"parts", "components", "apps"};
constexpr std::string_view library_view = "library";
constexpr std::string_view manifest_file = "view.vlp";
// The folder at a view's top that holds the contracts its nodes share (RV05): no node.
constexpr std::string_view contracts = "contracts";

std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

// A recipe of the library run as a view is in <library>/<kind>/<name>/view.vlp.
std::optional<std::filesystem::path> library_of(const std::filesystem::path &file) {
  const std::filesystem::path kind = file.parent_path().parent_path();
  if (kind.parent_path().filename() != "recipes" ||
      std::ranges::find(recipe_kinds, kind.filename().string()) == recipe_kinds.end())
    return std::nullopt;
  return kind.parent_path();
}

// The build tree mirrors a view by this name: its folder's, or the library's.
std::string name_of(const std::filesystem::path &file) {
  return library_of(file) ? std::string(library_view)
                          : file.parent_path().filename().string();
}

// Where a manifest's node names start (RV08): a view's folder, or in the library the
// folder of the recipe's kind, so a recipe's own node is the recipe's folder.
std::filesystem::path base_of(const View &view) {
  const std::filesystem::path folder = view.file.parent_path();
  return library_of(view.file) ? folder.parent_path() : folder;
}

// ui.panel as the path ui/panel.
std::filesystem::path path_of(std::string_view name) {
  std::filesystem::path path;
  for (std::size_t dot = name.find('.'); dot != std::string_view::npos;
       dot = name.find('.')) {
    path /= name.substr(0, dot);
    name.remove_prefix(dot + 1);
  }
  return path / name;
}

// The library's recipe of that name, in the first kind that holds it.
std::filesystem::path recipe_folder(const std::filesystem::path &library,
                                    const std::string &recipe) {
  for (const std::string_view kind : recipe_kinds)
    if (std::filesystem::is_directory(library / kind / recipe))
      return library / kind / recipe;
  return library / recipe_kinds.front() / recipe;
}

// A manifest reads # as a comment and a line as one value, and trims a value's edges,
// so a name holding those cannot be listed; nor are hidden files and editor backups.
bool listed(std::string_view name) {
  return !name.empty() && name.front() != '.' && !name.ends_with('~') &&
         name.find_first_of("#\n\r") == std::string_view::npos && trim(name) == name &&
         name != manifest_file;
}

// A folder that is not there holds no files.
std::vector<std::string> files_in(const std::filesystem::path &folder) {
  std::vector<std::string> files;
  std::error_code missing;
  for (const std::filesystem::directory_entry &entry :
       std::filesystem::directory_iterator(folder, missing)) {
    std::error_code gone; // a file an editor is replacing may vanish mid-listing
    std::string name = entry.path().filename().string();
    if (entry.is_regular_file(gone) && listed(name))
      files.push_back(std::move(name));
  }
  std::ranges::sort(files);
  return files;
}

// What a person wrote around one line of a manifest, so a save puts it back.
struct Note {
  std::string above; // the comment lines over it
  std::string after; // the comment that ends it, with the blanks before it
};
// By the place of the line each belongs to; "" holds the comments after the last line.
using Notes = std::map<std::string, Note, std::less<>>;

// A line's place: a section's header, or one word of it. A word that may repeat is told
// apart by what it names: a file by its name, a param by its key, a to by its port.
std::string
place(std::string_view section, std::string_view key = {}, std::string_view value = {}) {
  if (key == "param")
    value = trim(value.substr(0, value.find('=')));
  else if (key != "file" && key != "to")
    value = {};
  return std::format("{}\n{}\n{}", section, key, value);
}

// Reads one manifest line by line, and runs what it describes through the edits, so a
// loaded view and a typed one pass the same checks. Every mistake names its line (A02).
// Each section has a closed set of words (V04), so an unknown word is a mistake, never
// ignored.
class Reader {
public:
  explicit Reader(const std::filesystem::path &file)
      : _file(std::filesystem::absolute(file)) {}
  View read();
  const Notes &notes() const {
    return _notes;
  }

private:
  enum class Section { none, manifest, view, node, connection };

  [[noreturn]] void fail(std::string_view message) const;
  std::string here() const;
  // Runs an edit; its refusal names the given line.
  template <class Edit> void edit(std::size_t line, Edit &&change);
  void header(std::string_view text);
  void word(std::string_view key, std::string_view value);
  void view_word(std::string_view key, std::string_view value);
  void connection_word(std::string_view key, std::string_view value);
  void add_node();
  std::uint32_t number(std::string_view text) const;
  void note(const std::string &line, std::string_view text, std::string at);

  const std::filesystem::path _file;
  View _view;
  Section _section = Section::none;
  std::string _place; // the section's, as place() takes it
  std::size_t _line = 0;
  std::string _above; // comment lines since the last line with words
  Notes _notes;
  // The node a section describes, added once its section ends, from the section's line.
  std::optional<Node> _node;
  std::size_t _node_line = 0;
  // The views it hosts, and whether the last one's section named its file.
  std::vector<std::pair<Child, std::size_t>> _children;
  bool _child_file = false;
  // Joined once every node is in, so a connection may come before the nodes it joins.
  std::vector<std::pair<Connection, std::size_t>> _connections;
  bool _manifest = false;
  std::optional<std::uint32_t> _version;
};

void Reader::fail(std::string_view message) const {
  if (_line == 0)
    throw std::runtime_error(std::format("{}: {}", _file.string(), message));
  throw std::runtime_error(std::format("{}:{}: {}", _file.string(), _line, message));
}

// With its folder, so a node's errors say which of the views running holds it.
std::string Reader::here() const {
  return (_file.parent_path().filename() / _file.filename()).generic_string();
}

template <class Edit> void Reader::edit(std::size_t line, Edit &&change) {
  try {
    change();
  } catch (const std::runtime_error &refused) {
    _line = line;
    fail(refused.what());
  }
}

View Reader::read() {
  std::ifstream in(_file);
  if (!in)
    fail("cannot be read");
  _view.file = _file;
  _view.name = name_of(_file);
  for (std::string line; std::getline(in, line);) {
    ++_line;
    const std::string_view text = trim(std::string_view(line).substr(0, line.find('#')));
    if (text.empty()) {
      // A blank line before any comment is layout, which a save sets itself.
      if (!_above.empty() || line.find('#') != std::string::npos)
        _above.append(line).push_back('\n');
      continue;
    }
    if (text.front() == '[') {
      header(text);
      note(line, text, place(_place));
      continue;
    }
    const std::size_t equals = text.find('=');
    if (equals == std::string_view::npos)
      fail("expected `word = value`");
    const std::string_view key = trim(text.substr(0, equals));
    const std::string_view value = trim(text.substr(equals + 1));
    word(key, value);
    note(line, text, place(_place, key, value));
  }
  if (!_above.empty())
    _notes.emplace("", Note{.above = std::move(_above)});
  _line = 0;
  if (_version != manifest_version)
    fail(
        std::format("version {} is not one this build reads; it reads version {}, and no "
                    "migrate command exists yet (RV03)",
                    _version.value_or(0),
                    manifest_version));
  add_node();
  for (auto &[child, line] : _children)
    edit(line, [&] { Edits::child(_view, std::move(child)); });
  for (auto &[connection, line] : _connections)
    edit(line, [&] { Edits::connect(_view, std::move(connection)); });
  return std::move(_view);
}

void Reader::header(std::string_view text) {
  if (text.back() != ']')
    fail("a section header ends in ]");
  add_node();
  const std::string_view inside = trim(text.substr(1, text.size() - 2));
  const std::size_t space = inside.find_first_of(blanks);
  const std::string_view kind = inside.substr(0, space);
  std::string_view name =
      space == std::string_view::npos ? "" : trim(inside.substr(space));
  if (name.size() > 1 && name.front() == '"' && name.back() == '"')
    name = name.substr(1, name.size() - 2);
  else if (!name.empty())
    fail("a section's name is quoted: [node \"name\"]");
  _place = name.empty() ? std::string(kind) : std::format("{} {}", kind, name);
  const std::string where = std::format("{}:{}", here(), _line);
  if (kind == "manifest" && name.empty()) {
    if (_manifest)
      fail("[manifest] is given twice");
    _manifest = true;
    _section = Section::manifest;
  } else if (kind == "view" && !name.empty()) {
    _children.emplace_back(Child{.name = std::string(name),
                                 .file = _file.parent_path() / name / manifest_file,
                                 .where = where},
                           _line);
    _child_file = false;
    _section = Section::view;
  } else if (kind == "node" && !name.empty()) {
    _node.emplace(Node{.name = std::string(name), .where = where});
    _node_line = _line;
    _section = Section::node;
  } else if (kind == "connection" && !name.empty()) {
    _connections.emplace_back(Connection{.name = std::string(name)}, _line);
    _section = Section::connection;
  } else {
    fail(
        std::format("unknown section [{}]; the sections are [manifest], [view \"name\"], "
                    "[node \"name\"] and [connection \"name\"]",
                    inside));
  }
}

void Reader::word(std::string_view key, std::string_view value) {
  switch (_section) {
    case Section::manifest:
      if (key != "version")
        fail(std::format("unknown word {} in [manifest]; its one word is version", key));
      if (_version)
        fail("version is set twice");
      _version = number(value);
      return;
    case Section::view:
      return view_word(key, value);
    case Section::node:
      return edit(_line, [&] { Edits::word(*_node, key, value); });
    case Section::connection:
      return connection_word(key, value);
    case Section::none:
      fail("a word before any section; a manifest starts with [manifest]");
  }
}

// A file from the manifest's folder (RP02), which a save writes back the same way.
void Reader::view_word(std::string_view key, std::string_view value) {
  if (key != "file")
    fail(std::format("unknown word {} in a view; its one word is file", key));
  if (_child_file)
    fail("file is set twice");
  if (value.empty())
    fail("file has no value");
  _child_file = true;
  _children.back().first.file =
      (_file.parent_path() / std::filesystem::path(value)).lexically_normal();
}

void Reader::connection_word(std::string_view key, std::string_view value) {
  Connection &connection = _connections.back().first;
  if (key == "from") {
    if (!connection.from.node.empty())
      fail("from is set twice");
    edit(_line, [&] { connection.from = Edits::endpoint(value); });
  } else if (key == "to") {
    edit(_line, [&] { connection.to.push_back(Edits::endpoint(value)); });
  } else {
    fail(std::format("unknown word {} in a connection; its words are from and to", key));
  }
}

void Reader::add_node() {
  if (_node)
    edit(_node_line, [&] { Edits::add(_view, std::move(*_node)); });
  _node.reset();
}

std::uint32_t Reader::number(std::string_view text) const {
  std::uint32_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    fail(std::format("{} is not a whole number", text));
  return value;
}

// The comment lines since the last line with words belong to this line, and so does
// the comment that ends it.
void Reader::note(const std::string &line, std::string_view text, std::string at) {
  const auto end = static_cast<std::size_t>(text.data() + text.size() - line.data());
  _notes[std::move(at)] = {std::exchange(_above, {}),
                           line.find('#') == std::string::npos ? "" : line.substr(end)};
}

// One word of a section, as a save writes it.
struct Word {
  std::string_view key;
  std::string value;
};

// In the order a person reads a node: where it came from, what it runs and its files,
// then its settings.
std::vector<Word> words(const Node &node) {
  std::vector<Word> words;
  if (!node.recipe.empty())
    words.push_back({"recipe", node.recipe});
  if (!node.operator_name.empty())
    words.push_back({"operator", node.operator_name});
  for (const std::string &file : node.files)
    words.push_back({"file", file});
  if (node.invocations != 0)
    words.push_back({"invocations", std::to_string(node.invocations)});
  if (node.vertex_count != 0)
    words.push_back({"vertex_count", std::to_string(node.vertex_count)});
  if (node.instance_count != 0)
    words.push_back({"instance_count", std::to_string(node.instance_count)});
  for (const Param &param : node.params)
    words.push_back({"param", std::format("{}={}", param.key, param.value)});
  if (!node.log.empty())
    words.push_back({"log", node.log});
  return words;
}

// A child's file only when it is not <name>/view.vlp beside the manifest, from the
// manifest's folder.
std::vector<Word> words(const Child &child, const std::filesystem::path &folder) {
  if (child.file.lexically_normal() ==
      (folder / child.name / manifest_file).lexically_normal())
    return {};
  return {{"file", child.file.lexically_proximate(folder).generic_string()}};
}

std::vector<Word> words(const Connection &connection) {
  std::vector<Word> words{
      {"from", std::format("{}.{}", connection.from.node, connection.from.port)}};
  for (const Endpoint &to : connection.to)
    words.push_back({"to", std::format("{}.{}", to.node, to.port)});
  return words;
}

void put(std::string &text,
         const Notes &notes,
         const std::string &at,
         std::string_view line) {
  if (const auto found = notes.find(at); found != notes.end())
    text.append(found->second.above).append(line).append(found->second.after);
  else
    text.append(line);
  text.push_back('\n');
}

// A blank line before every section but the first, and its keys aligned, as a person
// lays a manifest out.
void section(std::string &text,
             const Notes &notes,
             std::string_view kind,
             std::string_view name,
             const std::vector<Word> &words) {
  const std::string at =
      name.empty() ? std::string(kind) : std::format("{} {}", kind, name);
  if (!text.empty())
    text.push_back('\n');
  put(text,
      notes,
      place(at),
      name.empty() ? std::format("[{}]", kind) : std::format("[{} \"{}\"]", kind, name));
  std::size_t width = 0;
  for (const Word &word : words)
    width = std::max(width, word.key.size());
  for (const Word &word : words)
    put(text,
        notes,
        place(at, word.key, word.value),
        std::format("{:<{}} = {}", word.key, width, word.value));
}

// The view in the words load reads, with what a person wrote around each line.
std::string text(const View &view, const Notes &notes) {
  std::string text;
  section(text, notes, "manifest", {}, {{"version", std::to_string(manifest_version)}});
  for (const Child &child : view.children)
    section(text, notes, "view", child.name, words(child, view.file.parent_path()));
  for (const Node &node : view.nodes)
    section(text, notes, "node", node.name, words(node));
  for (const Connection &connection : view.connections)
    section(text, notes, "connection", connection.name, words(connection));
  if (const auto end = notes.find(""); end != notes.end())
    text.append("\n").append(end->second.above);
  return text;
}

// Each folder below a folder that no node or hosted view names, named as a node inside
// the parent would be. Inside a node, a folder is a node too.
void unnamed_below(const View &view,
                   const std::filesystem::path &folder,
                   const std::string &parent,
                   std::vector<std::string> &notes) {
  std::error_code missing;
  for (const std::filesystem::directory_entry &entry :
       std::filesystem::directory_iterator(folder, missing)) {
    std::error_code gone;
    const std::string name = entry.path().filename().string();
    const bool top = parent.empty();
    if (!entry.is_directory(gone) || name.starts_with('.') ||
        std::filesystem::exists(entry.path() / manifest_file, gone) ||
        (top &&
         (name == contracts ||
          std::ranges::find(view.children, name, &Child::name) != view.children.end())))
      continue;
    const std::string node = top ? name : parent + '.' + name;
    if (std::ranges::find(view.nodes, node, &Node::name) == view.nodes.end())
      notes.push_back(std::format("folder {}/ is no node's, but every folder is a node: "
                                  "give it a section [node \"{}\"], or move it out",
                                  path_of(node).generic_string(),
                                  node));
    else
      unnamed_below(view, entry.path(), node, notes);
  }
}

// Where a node was written, before its error's text; nothing for a line typed.
std::string at(const Node &node) {
  return node.where.empty() ? std::string() : node.where + ": ";
}

View flattened(View view,
               const std::filesystem::path &root,
               std::vector<std::string> &using_recipes);

// A param of a node that uses a recipe sets the recipe's own node, or with a dotted key
// a node inside it, by its name inside the recipe: panel.rows=4.
void set_params(View &recipe, const Node &user) {
  for (const Param &param : user.params) {
    const std::size_t dot = param.key.rfind('.');
    const std::string name =
        dot == std::string::npos
            ? user.recipe
            : std::format("{}.{}", user.recipe, param.key.substr(0, dot));
    const auto node = std::ranges::find(recipe.nodes, name, &Node::name);
    if (node == recipe.nodes.end())
      throw std::runtime_error(
          std::format("it sets param {}, but recipe {} has no node {}",
                      param.key,
                      user.recipe,
                      name));
    const std::string key = param.key.substr(dot + 1);
    const auto found = std::ranges::find(node->params, key, &Param::key);
    if (found != node->params.end())
      found->value = param.value;
    else
      node->params.push_back({key, param.value});
    node->where = user.where;
  }
}

// Into the view, the recipe a node uses, as that node and the nodes inside it (V11): a
// recipe is one node named like its folder, and the nodes inside it.
void unfold(View &view,
            const Node &user,
            const std::filesystem::path &root,
            std::vector<std::string> &using_recipes) {
  if (std::ranges::find(using_recipes, user.recipe) != using_recipes.end())
    throw std::runtime_error(
        std::format("the recipes it uses form a cycle through {} (RV06)", user.recipe));
  View recipe = Manifest::load(recipe_folder(root, user.recipe) / manifest_file);
  Manifest::refresh(recipe);
  using_recipes.push_back(user.recipe);
  recipe = flattened(std::move(recipe), root, using_recipes);
  using_recipes.pop_back();
  const std::string inside = user.recipe + '.';
  if (std::ranges::find(recipe.nodes, user.recipe, &Node::name) == recipe.nodes.end())
    throw std::runtime_error(std::format(
        "recipe {} has no node {}: a recipe is one node named like its folder, and the "
        "nodes inside it",
        user.recipe,
        user.recipe));
  for (const Node &node : recipe.nodes)
    if (node.name != user.recipe && !node.name.starts_with(inside))
      throw std::runtime_error(std::format(
          "recipe {} holds node {}, which is not inside its node {}: a recipe is one "
          "node, and the nodes inside it",
          user.recipe,
          node.name,
          user.recipe));
  set_params(recipe, user);
  const auto named = [&](std::string &name) {
    name = user.name + name.substr(user.recipe.size());
  };
  for (Node &node : recipe.nodes) {
    if (node.name == user.recipe && !user.log.empty())
      node.log = user.log;
    named(node.name);
    view.nodes.push_back(std::move(node));
  }
  for (Connection &connection : recipe.connections) {
    connection.name = std::format("{}.{}", user.name, connection.name);
    named(connection.from.node);
    for (Endpoint &to : connection.to)
      named(to.node);
    view.connections.push_back(std::move(connection));
  }
}

// Each node with its folder, and each node that uses a recipe unfolded in its place.
View flattened(View view,
               const std::filesystem::path &root,
               std::vector<std::string> &using_recipes) {
  const std::filesystem::path base = base_of(view);
  for (Node &node : std::exchange(view.nodes, {})) {
    if (!node.uses()) {
      node.folder = base / path_of(node.name);
      node.module = node.folder.lexically_relative(root);
      view.nodes.push_back(std::move(node));
      continue;
    }
    try {
      unfold(view, node, root, using_recipes);
    } catch (const std::runtime_error &failure) {
      throw std::runtime_error(
          std::format("{}node {}: {}", at(node), node.name, failure.what()));
    }
  }
  return view;
}

// Only the library's own recipes use others as they are (V11); a view holds its own
// copies (V03).
void check_copies(const View &view) {
  for (const Node &node : view.nodes)
    if (node.uses())
      throw std::runtime_error(
          std::format("{}node {} uses recipe {} as it is, as only the library's own "
                      "recipes do (V11); a view holds its own copy (V03), which recipe "
                      "drop {} {} makes",
                      at(node),
                      node.name,
                      node.recipe,
                      node.recipe,
                      node.name));
}

// A node inside another is inside a node of the view, whose folder holds its own.
void check_inside(const View &flat) {
  for (const Node &node : flat.nodes) {
    const std::size_t dot = node.name.rfind('.');
    if (dot != std::string::npos &&
        std::ranges::find(flat.nodes, node.name.substr(0, dot), &Node::name) ==
            flat.nodes.end())
      throw std::runtime_error(std::format("{}node {} is inside {}, which is no node",
                                           at(node),
                                           node.name,
                                           node.name.substr(0, dot)));
  }
}

} // namespace

View Manifest::load(const std::filesystem::path &file) {
  return Reader(file).read();
}

View Manifest::empty(const std::filesystem::path &file) {
  const std::filesystem::path absolute = std::filesystem::absolute(file);
  return {.name = name_of(absolute), .file = absolute};
}

// A node's first list says nothing new, so only a list that changes is noted.
std::vector<std::string> Manifest::refresh(View &view) {
  std::vector<std::string> notes;
  const std::filesystem::path base = base_of(view);
  for (Node &node : view.nodes) {
    if (node.uses())
      continue;
    const std::filesystem::path folder = path_of(node.name);
    std::vector<std::string> files = files_in(base / folder);
    if (node.files.empty()) {
      node.files = std::move(files);
      continue;
    }
    for (const std::string &file : files)
      if (std::ranges::find(node.files, file) == node.files.end())
        notes.push_back(
            std::format("node {}: {} is in {}/ now, so it is one of its files",
                        node.name,
                        file,
                        folder.generic_string()));
    for (const std::string &file : node.files)
      if (std::ranges::find(files, file) == files.end())
        notes.push_back(std::format(
            "node {}: {} is no longer in {}/", node.name, file, folder.generic_string()));
    node.files = std::move(files);
  }
  return notes;
}

// In the library, from each recipe's own folder; in a view, from the view's.
std::vector<std::string> Manifest::unnamed(const View &view) {
  std::vector<std::string> notes;
  if (!library_of(view.file)) {
    unnamed_below(view, base_of(view), {}, notes);
    return notes;
  }
  for (const Node &node : view.nodes)
    if (node.name.find('.') == std::string::npos && !node.uses())
      unnamed_below(view, base_of(view) / node.name, node.name, notes);
  return notes;
}

// A connection may name a node inside a recipe a node uses, which only the view with
// those recipes unfolded shows.
View Manifest::flatten(const View &view) {
  if (!library_of(view.file))
    check_copies(view);
  std::vector<std::string> using_recipes;
  View flat = flattened(view, root(view), using_recipes);
  check_inside(flat);
  for (const Connection &connection : flat.connections) {
    const auto check = [&](const Endpoint &end) {
      if (std::ranges::find(flat.nodes, end.node, &Node::name) == flat.nodes.end())
        throw std::runtime_error(std::format(
            "connection {} joins {}, which is no node", connection.name, end.node));
    };
    check(connection.from);
    std::ranges::for_each(connection.to, check);
  }
  return flat;
}

std::filesystem::path Manifest::root(const View &view) {
  return library_of(view.file).value_or(view.file.parent_path());
}

// The comments come from the file being replaced, read as a load reads it.
void Manifest::save(const View &view) {
  std::error_code missing;
  if (!std::filesystem::exists(view.file, missing))
    return Files::save(view.file, text(view, {}));
  Reader reader(view.file);
  reader.read();
  Files::save(view.file, text(view, reader.notes()));
}

} // namespace VP
