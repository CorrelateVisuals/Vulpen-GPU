#include "runtime/Manifest.h"

#include "baseclasses/Platform.h"
#include "runtime/Edits.h"
#include "runtime/View.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VP {

namespace {

constexpr std::uint32_t manifest_version = 1;
constexpr std::string_view blanks = " \t\r";

std::string_view trim(std::string_view text) {
  const std::size_t first = text.find_first_not_of(blanks);
  if (first == std::string_view::npos)
    return {};
  return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

// What a person wrote around one line of a manifest, so a save puts it back.
struct Note {
  std::string above; // the comment lines over it
  std::string after; // the comment that ends it, with the blanks before it
};
// By the place of the line each belongs to; "" holds the comments after the last line.
using Notes = std::map<std::string, Note, std::less<>>;

// A line's place: a section's header, or one word of it. A word that may repeat is told
// apart by what it names: a shader by its file, a param by its key, a to by its port.
std::string
place(std::string_view section, std::string_view key = {}, std::string_view value = {}) {
  if (key == "param")
    value = trim(value.substr(0, value.find('=')));
  else if (key != "shader" && key != "to")
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
  enum class Section { none, manifest, deploy, node, connection };

  [[noreturn]] void fail(std::string_view message) const;
  // Runs an edit; its refusal names the given line.
  template <class Edit> void edit(std::size_t line, Edit &&change);
  void header(std::string_view text);
  void word(std::string_view key, std::string_view value);
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
  // Joined once every node is in, so a connection may come before the nodes it joins.
  std::vector<std::pair<Connection, std::size_t>> _connections;
  std::vector<std::pair<Deploy, std::size_t>> _deploys; // added before the connections
  bool _manifest = false;
  std::optional<std::uint32_t> _version;
};

void Reader::fail(std::string_view message) const {
  if (_line == 0)
    throw std::runtime_error(std::format("{}: {}", _file.string(), message));
  throw std::runtime_error(std::format("{}:{}: {}", _file.string(), _line, message));
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
  _view.name = _file.parent_path().filename().string();
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
  for (auto &[deploy, line] : _deploys)
    edit(line, [&] { Edits::deploy(_view, std::move(deploy)); });
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
  if (kind == "manifest" && name.empty()) {
    if (_manifest)
      fail("[manifest] is given twice");
    _manifest = true;
    _section = Section::manifest;
  } else if (kind == "node" && !name.empty()) {
    _node.emplace(Node{.name = std::string(name),
                       .where = std::format("{}:{}", _file.filename().string(), _line)});
    _node_line = _line;
    _section = Section::node;
  } else if (kind == "connection" && !name.empty()) {
    _connections.emplace_back(Connection{.name = std::string(name)}, _line);
    _section = Section::connection;
  } else if (kind == "deploy" && !name.empty()) {
    _deploys.emplace_back(
        Deploy{.name = std::string(name),
               .where = std::format("{}:{}", _file.filename().string(), _line)},
        _line);
    _section = Section::deploy;
  } else {
    fail(std::format(
        "unknown section [{}]; the sections are [manifest], [deploy \"name\"], "
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
    case Section::deploy:
      return edit(_line, [&] { Edits::word(_deploys.back().first, key, value); });
    case Section::node:
      return edit(_line, [&] { Edits::word(*_node, key, value); });
    case Section::connection:
      return connection_word(key, value);
    case Section::none:
      fail("a word before any section; a manifest starts with [manifest]");
  }
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

// In the order a person reads a node: where it comes from, what it runs, then its
// settings.
std::vector<Word> words(const Node &node) {
  std::vector<Word> words{{"recipe", node.recipe}};
  if (!node.operator_name.empty())
    words.push_back({"operator", node.operator_name});
  for (const std::string &shader : node.shaders)
    words.push_back({"shader", shader});
  if (node.invocations != 0)
    words.push_back({"invocations", std::to_string(node.invocations)});
  for (const Param &param : node.params)
    words.push_back({"param", std::format("{}={}", param.key, param.value)});
  if (!node.log.empty())
    words.push_back({"log", node.log});
  return words;
}

std::vector<Word> words(const Deploy &deploy) {
  std::vector<Word> words{{"recipe", deploy.recipe}};
  for (const Param &param : deploy.params)
    words.push_back({"param", std::format("{}={}", param.key, param.value)});
  return words;
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
  for (const Deploy &deploy : view.deploys)
    section(text, notes, "deploy", deploy.name, words(deploy));
  for (const Node &node : view.nodes)
    section(text, notes, "node", node.name, words(node));
  for (const Connection &connection : view.connections)
    section(text, notes, "connection", connection.name, words(connection));
  if (const auto end = notes.find(""); end != notes.end())
    text.append("\n").append(end->second.above);
  return text;
}

View flattened(View view,
               const std::filesystem::path &recipes,
               std::vector<std::string> &deploying);

// A deploy's params set its recipe's nodes, so a node they set was last changed there.
void set_params(View &recipe, const Deploy &deploy) {
  for (const Param &param : deploy.params) {
    const std::size_t dot = param.key.rfind('.');
    const std::string_view name = std::string_view(param.key).substr(0, dot);
    const auto node = std::ranges::find(recipe.nodes, name, &Node::name);
    if (node == recipe.nodes.end())
      throw std::runtime_error(
          std::format("it sets param {}, but recipe {} has no node {}",
                      param.key,
                      deploy.recipe,
                      name));
    const std::string key = param.key.substr(dot + 1);
    const auto found = std::ranges::find(node->params, key, &Param::key);
    if (found != node->params.end())
      found->value = param.value;
    else
      node->params.push_back({key, param.value});
    node->where = deploy.where;
  }
}

// Into the view, the deploy's recipe under the deploy's name.
void unfold(View &view,
            const Deploy &deploy,
            const std::filesystem::path &recipes,
            std::vector<std::string> &deploying) {
  if (std::ranges::find(deploying, deploy.recipe) != deploying.end())
    throw std::runtime_error(
        std::format("the deploys form a cycle through recipe {} (RV06)", deploy.recipe));
  View recipe = Manifest::load(recipes / deploy.recipe / "view.vlp");
  for (Node &node : recipe.nodes)
    node.where = std::format("recipes/{}/{}", deploy.recipe, node.where);
  deploying.push_back(deploy.recipe);
  recipe = flattened(std::move(recipe), recipes, deploying);
  deploying.pop_back();
  set_params(recipe, deploy);
  const auto named = [&](std::string &name) { name.insert(0, deploy.name + "."); };
  for (Node &node : recipe.nodes) {
    named(node.name);
    view.nodes.push_back(std::move(node));
  }
  for (Connection &connection : recipe.connections) {
    named(connection.name);
    named(connection.from.node);
    for (Endpoint &to : connection.to)
      named(to.node);
    view.connections.push_back(std::move(connection));
  }
}

View flattened(View view,
               const std::filesystem::path &recipes,
               std::vector<std::string> &deploying) {
  for (const Deploy &deploy : view.deploys) {
    try {
      unfold(view, deploy, recipes, deploying);
    } catch (const std::runtime_error &failure) {
      throw std::runtime_error(std::format("{}{}deploy {}: {}",
                                           deploy.where,
                                           deploy.where.empty() ? "" : ": ",
                                           deploy.name,
                                           failure.what()));
    }
  }
  view.deploys.clear();
  return view;
}

} // namespace

View Manifest::load(const std::filesystem::path &file) {
  return Reader(file).read();
}

// A connection may name a deploy's node that its recipe lacks, which only the view
// with the deploys unfolded shows.
View Manifest::flatten(const View &view) {
  std::vector<std::string> deploying;
  View flat = flattened(view, view.file.parent_path() / "recipes", deploying);
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
