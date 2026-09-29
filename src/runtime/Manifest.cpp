#include "runtime/Manifest.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string_view>

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

// Reads one manifest line by line; every mistake names its line (A02). Each section has
// a closed set of words (V04), so an unknown word is a mistake, never ignored.
class Reader {
public:
  explicit Reader(const std::filesystem::path &file)
      : _file(std::filesystem::absolute(file)) {}
  View read();

private:
  enum class Section { none, manifest, node, connection };

  [[noreturn]] void fail(std::string_view message) const;
  void header(std::string_view text);
  void word(std::string_view key, std::string_view value);
  void node_word(Node &node, std::string_view key, std::string_view value);
  void
  connection_word(Connection &connection, std::string_view key, std::string_view value);
  void once(std::string &slot, std::string_view key, std::string_view value) const;
  Endpoint endpoint(std::string_view text) const;
  std::uint32_t number(std::string_view text) const;
  void check() const;

  const std::filesystem::path _file;
  View _view;
  Section _section = Section::none;
  std::size_t _line = 0;
  std::uint32_t _version = 0;
};

void Reader::fail(std::string_view message) const {
  if (_line == 0)
    throw std::runtime_error(std::format("{}: {}", _file.string(), message));
  throw std::runtime_error(std::format("{}:{}: {}", _file.string(), _line, message));
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
  check();
  return std::move(_view);
}

void Reader::header(std::string_view text) {
  if (text.back() != ']')
    fail("a section header ends in ]");
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
    _section = Section::manifest;
  } else if (kind == "node" && !name.empty()) {
    _view.nodes.push_back({.name = std::string(name)});
    _section = Section::node;
  } else if (kind == "connection" && !name.empty()) {
    _view.connections.push_back({.name = std::string(name)});
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
      _version = number(value);
      return;
    case Section::node:
      return node_word(_view.nodes.back(), key, value);
    case Section::connection:
      return connection_word(_view.connections.back(), key, value);
    case Section::none:
      fail("a word before any section; a manifest starts with [manifest]");
  }
}

void Reader::node_word(Node &node, std::string_view key, std::string_view value) {
  if (key == "recipe") {
    once(node.recipe, key, value);
  } else if (key == "operator") {
    once(node.operator_name, key, value);
  } else if (key == "shader") {
    node.shaders.emplace_back(value);
  } else if (key == "log") {
    once(node.log, key, value);
  } else if (key == "invocations") {
    if (node.invocations != 0)
      fail("invocations is set twice");
    node.invocations = number(value);
  } else if (key == "param") {
    const std::size_t equals = value.find('=');
    if (equals == std::string_view::npos)
      fail("a param is written `param = name=value`");
    node.params.push_back({std::string(trim(value.substr(0, equals))),
                           std::string(trim(value.substr(equals + 1)))});
  } else {
    fail(std::format("unknown word {} in a node; its words are recipe, operator, shader, "
                     "invocations, param and log",
                     key));
  }
}

void Reader::connection_word(Connection &connection,
                             std::string_view key,
                             std::string_view value) {
  if (key == "from") {
    if (!connection.from.node.empty())
      fail("from is set twice");
    connection.from = endpoint(value);
  } else if (key == "to") {
    connection.to.push_back(endpoint(value));
  } else {
    fail(std::format("unknown word {} in a connection; its words are from and to", key));
  }
}

void Reader::once(std::string &slot, std::string_view key, std::string_view value) const {
  if (!slot.empty())
    fail(std::format("{} is set twice", key));
  slot = value;
}

Endpoint Reader::endpoint(std::string_view text) const {
  const std::size_t dot = text.find('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 == text.size())
    fail(std::format("{} is not node.port", text));
  return {std::string(text.substr(0, dot)), std::string(text.substr(dot + 1))};
}

std::uint32_t Reader::number(std::string_view text) const {
  std::uint32_t value = 0;
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    fail(std::format("{} is not a whole number", text));
  return value;
}

// What no single line can show: the version, and names that must match across entries.
void Reader::check() const {
  if (_version != manifest_version)
    fail(
        std::format("version {} is not one this build reads; it reads version {}, and no "
                    "migrate command exists yet (RV03)",
                    _version,
                    manifest_version));
  const auto named = [&](std::string_view name) {
    return std::ranges::count(_view.nodes, name, &Node::name);
  };
  for (const Node &node : _view.nodes) {
    if (named(node.name) > 1)
      fail(std::format("two nodes are named {}", node.name));
    if (node.operator_name.empty() && node.shaders.empty())
      fail(std::format("node {} runs nothing: give it an operator, a shader or both",
                       node.name));
    if (node.recipe.empty())
      fail(std::format("node {} names no recipe to find its C++ and GLSL in", node.name));
    if (!node.shaders.empty() && node.invocations == 0)
      fail(std::format("node {} runs a shader, so it needs invocations", node.name));
  }
  for (const Connection &connection : _view.connections) {
    if (connection.from.node.empty() || connection.to.empty())
      fail(
          std::format("connection {} needs a from and at least one to", connection.name));
    if (named(connection.from.node) == 0)
      fail(std::format("connection {} is from {}, which is no node",
                       connection.name,
                       connection.from.node));
    for (const Endpoint &to : connection.to)
      if (named(to.node) == 0)
        fail(std::format(
            "connection {} is to {}, which is no node", connection.name, to.node));
  }
}

} // namespace

View Manifest::load(const std::filesystem::path &file) {
  return Reader(file).read();
}

} // namespace VP
