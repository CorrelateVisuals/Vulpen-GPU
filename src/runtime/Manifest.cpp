#include "runtime/Manifest.h"

#include "baseclasses/Platform.h"
#include "runtime/Edits.h"
#include "runtime/View.h"

#include <charconv>
#include <format>
#include <fstream>
#include <optional>
#include <stdexcept>
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

// Reads one manifest line by line, and runs what it describes through the edits, so a
// loaded view and a typed one pass the same checks. Every mistake names its line (A02).
// Each section has a closed set of words (V04), so an unknown word is a mistake, never
// ignored.
class Reader {
public:
  explicit Reader(const std::filesystem::path &file)
      : _file(std::filesystem::absolute(file)) {}
  View read();

private:
  enum class Section { none, manifest, node, connection };

  [[noreturn]] void fail(std::string_view message) const;
  // Runs an edit; its refusal names the given line.
  template <class Edit> void edit(std::size_t line, Edit &&change);
  void header(std::string_view text);
  void word(std::string_view key, std::string_view value);
  void connection_word(std::string_view key, std::string_view value);
  void add_node();
  std::uint32_t number(std::string_view text) const;

  const std::filesystem::path _file;
  View _view;
  Section _section = Section::none;
  std::size_t _line = 0;
  // The node a section describes, added once its section ends, from the section's line.
  std::optional<Node> _node;
  std::size_t _node_line = 0;
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
    if (text.empty())
      continue;
    if (text.front() == '[') {
      header(text);
      continue;
    }
    const std::size_t equals = text.find('=');
    if (equals == std::string_view::npos)
      fail("expected `word = value`");
    word(trim(text.substr(0, equals)), trim(text.substr(equals + 1)));
  }
  _line = 0;
  if (_version != manifest_version)
    fail(
        std::format("version {} is not one this build reads; it reads version {}, and no "
                    "migrate command exists yet (RV03)",
                    _version.value_or(0),
                    manifest_version));
  add_node();
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
  if (kind == "manifest" && name.empty()) {
    if (_manifest)
      fail("[manifest] is given twice");
    _manifest = true;
    _section = Section::manifest;
  } else if (kind == "node" && !name.empty()) {
    _node.emplace(Node{.name = std::string(name)});
    _node_line = _line;
    _section = Section::node;
  } else if (kind == "connection" && !name.empty()) {
    _connections.emplace_back(Connection{.name = std::string(name)}, _line);
    _section = Section::connection;
  } else {
    fail(std::format(
        "unknown section [{}]; the sections are [manifest], [node \"name\"] and "
        "[connection \"name\"]",
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

} // namespace

View Manifest::load(const std::filesystem::path &file) {
  return Reader(file).read();
}

} // namespace VP
