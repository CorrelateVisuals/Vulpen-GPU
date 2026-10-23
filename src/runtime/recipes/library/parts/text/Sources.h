#pragma once

#include "shared/Paths.h"
#include "Syntax.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// Only Text.cpp includes this header, so what it holds stays in the unnamed namespace of
// the one file that builds the part, as the class does: what stands at the caret, a path
// or a name, and where a row declares or defines a name, which jump looks for. No
// compiler is asked, as the relations part asks none: the lines say it.
namespace {

// A place a name comes from: the file, absolute, the row, and the byte of it where the
// name starts, and how long it is.
struct Source {
  std::string file;
  std::size_t row = 0;
  std::size_t column = 0;
  std::size_t length = 0;
};

// Words after which a name is brought in rather than used: a type, an alias, a namespace,
// a concept, a macro, or in Python a function or a class.
constexpr std::string_view introducers =
    " class struct union enum namespace concept using define def ";
// Words a name past which is used, though a word stands before it, as return x does, or
// is a base, as public x is.
constexpr std::string_view users =
    " return new delete throw case else do goto co_return co_yield co_await sizeof "
    "alignof decltype typeid not and or if while for switch catch typename template "
    "operator public private protected virtual ";
// What may follow a name declared: its parameters, its value, its body, its size, its
// end, the next of a list, a parameter list's end, a base or a bit field.
constexpr std::string_view declared_end = "(;={[,):";
// The characters a path a line names is made of, as an include's, a link's or a file
// word's in a manifest.
constexpr std::string_view path_bytes = "./-+~@";

bool name_byte(char byte) {
  return letter(byte) || digit(byte);
}

// The name the caret stands in or just past: letters, digits and _; none for none.
std::optional<std::pair<std::size_t, std::size_t>> name_at(std::string_view row,
                                                            std::size_t column) {
  column = std::min(column, row.size());
  if ((column == row.size() || !name_byte(row[column])) &&
      (column == 0 || !name_byte(row[column - 1])))
    return std::nullopt;
  std::size_t from = column;
  std::size_t to = column;
  while (from > 0 && name_byte(row[from - 1]))
    --from;
  while (to < row.size() && name_byte(row[to]))
    ++to;
  return std::pair{from, to};
}

// The run of a path's characters the caret stands in, as contracts/Rect.h in an include
// or a link; empty where it holds neither a / nor a ., nor a -, as a recipe's name.
std::string_view path_at(std::string_view row, std::size_t column) {
  const auto in_path = [](char byte) {
    return name_byte(byte) || path_bytes.find(byte) != std::string_view::npos;
  };
  column = std::min(column, row.size());
  std::size_t from = column;
  std::size_t to = column;
  while (from > 0 && in_path(row[from - 1]))
    --from;
  while (to < row.size() && in_path(row[to]))
    ++to;
  const std::string_view run = row.substr(from, to - from);
  return run.find_first_of("/.-") == std::string_view::npos ? std::string_view() : run;
}

std::string_view trimmed_end(std::string_view text) {
  const std::size_t last = text.find_last_not_of(" \t");
  return last == std::string_view::npos ? std::string_view() : text.substr(0, last + 1);
}

// The name ending a text, past what holds no name; empty for none.
std::string_view last_name(std::string_view text) {
  std::size_t from = text.size();
  while (from > 0 && name_byte(text[from - 1]))
    --from;
  return text.substr(from);
}

// The text before a name, past the qualifiers it is defined under, as Text:: of
// Text::cook, and past an alignas, as a contract's struct alignas(8) Rect holds;
// whether a qualifier was passed.
std::pair<std::string_view, bool> lead_of(std::string_view code, std::size_t at) {
  std::size_t before = at;
  bool qualified = false;
  while (before >= 2 && code.substr(before - 2, 2) == "::") {
    qualified = true;
    before -= 2;
    while (before > 0 && name_byte(code[before - 1]))
      --before;
  }
  std::string_view lead = trimmed_end(code.substr(0, before));
  if (lead.ends_with(')'))
    if (const std::size_t open = lead.rfind("alignas("); open != std::string_view::npos)
      lead = trimmed_end(lead.substr(0, open));
  return {lead, qualified};
}

// Where a row declares or defines the name, standing whole: past a word bringing it in,
// as class, using with = after it, or #define, or past a type, as a variable, a member,
// a parameter or a function is declared, a member function perhaps under its class's
// name, as Text::cook; and before what may follow a name declared. Never past . or ->,
// nor past a word that uses it, as return or public do; nor what a comment past // says.
// In a manifest, its node or connection section, by the longest of its dotted names the
// name starts with. Where the name declared starts, and how long it is; none for none.
std::optional<std::pair<std::size_t, std::size_t>>
declares(std::string_view row, std::string_view name, Syntax syntax) {
  if (syntax == Syntax::keyed) {
    for (std::string_view named = name;;) {
      for (const std::string_view section : {"[node \"", "[connection \""})
        if (row.starts_with(section) && row.substr(section.size()).starts_with(named) &&
            row.substr(section.size() + named.size()).starts_with("\"]"))
          return std::pair{section.size(), named.size()};
      const std::size_t dot = named.rfind('.');
      if (dot == std::string_view::npos)
        return std::nullopt;
      named = named.substr(0, dot);
    }
  }
  const std::string_view code = row.substr(0, row.find("//"));
  for (std::size_t at = code.find(name); at != std::string_view::npos;
       at = code.find(name, at + 1)) {
    const std::size_t end = at + name.size();
    if ((at > 0 && name_byte(code[at - 1])) ||
        (end < code.size() && name_byte(code[end])))
      continue;
    const auto [lead, qualified] = lead_of(code, at);
    const std::size_t next = code.find_first_not_of(" \t", end);
    const char follows = next == std::string_view::npos ? '\0' : code[next];
    if (lead.ends_with('.') || lead.ends_with("->"))
      continue;
    const std::string_view word = last_name(lead);
    const std::string_view before_word =
        last_name(trimmed_end(lead.substr(0, lead.size() - word.size())));
    const bool brought = listed(introducers, word) && !qualified &&
                         (word != "using" || follows == '=') &&
                         (word != "namespace" || before_word != "using") &&
                         (word != "define" || lead.ends_with("#define"));
    const bool typed =
        (!word.empty() && !listed(users, word) && !listed(introducers, word)) ||
        lead.ends_with('>') || lead.ends_with('*') || lead.ends_with('&');
    const bool ends =
        follows != '\0' && declared_end.find(follows) != std::string_view::npos;
    if (brought || (typed && ends))
      return std::pair{at, name.size()};
  }
  return std::nullopt;
}

// The file or folder a path the caret stands in names, from the file's folder or one
// holding it, as an include reaches one; in a manifest, past its recipe word, the
// manifest of the recipe a node uses. Empty for none.
std::string named_path(Listed &listed,
                       const std::filesystem::path &folder,
                       std::string_view line,
                       std::size_t column,
                       Syntax syntax) {
  if (const std::string_view path = path_at(line, column); !path.empty())
    if (std::string found = resolved(listed, folder, path, true); !found.empty())
      return found;
  constexpr std::string_view recipe_word = "recipe";
  const std::size_t start = line.find_first_not_of(" \t");
  const std::size_t equals = line.find('=');
  if (syntax != Syntax::keyed || start == std::string_view::npos ||
      !line.substr(start).starts_with(recipe_word) || equals == std::string_view::npos ||
      column <= equals)
    return {};
  const std::size_t value = line.find_first_not_of(" \t", equals + 1);
  if (value == std::string_view::npos)
    return {};
  const std::string_view word =
      line.substr(value, line.find_first_of(" \t", value) - value);
  const std::string found = recipe(listed, folder, word);
  return found.empty() ? found : found + "/view.vlp";
}

// The headers a row includes by a quoted name, as C++ and GLSL do; empty for none.
std::string_view included(std::string_view row) {
  const std::size_t hash = row.find_first_not_of(" \t");
  if (hash == std::string_view::npos || !row.substr(hash).starts_with("#include"))
    return {};
  const std::size_t open = row.find('"', hash);
  const std::size_t close =
      open == std::string_view::npos ? open : row.find('"', open + 1);
  return close == std::string_view::npos ? std::string_view()
                                         : row.substr(open + 1, close - open - 1);
}

} // namespace
