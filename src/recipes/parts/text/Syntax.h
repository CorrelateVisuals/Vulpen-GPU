#pragma once

#include "contracts/Palette.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

// Only Text.h includes this header, so what it holds stays in the unnamed namespace of
// the one file that builds the part, as the class does.
namespace {

// How a file's text is told apart: C++ and GLSL, which nodes are written in, by their
// kinds of words; a manifest or an .ini by its keys; Python, CMake and shell, which the
// tooling is written in (RC00), by theirs; any other not at all.
enum class Syntax : std::uint8_t { plain, code, keyed, python, cmake, shell };

// What a row leaves open at its end, so the next row starts inside it: a block comment,
// or a string in three quotes, as Python's.
enum class Open : std::uint8_t { none, comment, double_quotes, single_quotes };

// The words of each syntax, and the files of each, a blank before and after each, so a
// word is found whole.
constexpr std::string_view keywords =
    " alignas alignof auto break case catch class concept const consteval constexpr"
    " constinit const_cast continue co_await co_return co_yield decltype default delete"
    " do dynamic_cast else enum explicit export extern false final for friend goto if"
    " inline mutable namespace new noexcept nullptr operator override private protected"
    " public reinterpret_cast requires return sizeof static static_assert static_cast"
    " struct switch template this throw true try typedef typeid typename union using"
    " virtual volatile while attribute buffer centroid coherent discard flat highp in"
    " inout invariant layout lowp mediump noperspective out patch precision readonly"
    " restrict sample shared smooth subroutine uniform varying writeonly ";
constexpr std::string_view types = " void bool char int unsigned signed short long float"
                                   " double uint sampler sampler2D texture2D image2D ";
constexpr std::string_view python_keywords =
    " False None True and as assert async await break case class continue def del elif"
    " else except finally for from global if import in is lambda match nonlocal not or"
    " pass raise return try while with yield ";
constexpr std::string_view python_types =
    " bool bytes dict float frozenset int list object set str tuple ";
constexpr std::string_view shell_keywords =
    " case declare do done elif else esac exit export fi for function if in local"
    " readonly return select shift then until unset while ";
constexpr std::string_view coded = " .cpp .h .hpp .cc .c .glsl .vert .frag .comp ";
constexpr std::string_view keyed = " .vlp .ini ";
constexpr std::string_view cmake_file = "CMakeLists.txt";
constexpr std::string_view exponents = "eEpP"; // a sign after one goes on a number
// What a shell names a variable with that is no name: $1, $#, $? and the like.
constexpr std::string_view shell_specials = "0123456789#?@*$!-";

// Whether the blank-separated words hold the word whole.
bool listed(std::string_view words, std::string_view word) {
  for (std::size_t at = words.find(word); at != std::string_view::npos && !word.empty();
       at = words.find(word, at + 1))
    if (words[at - 1] == ' ' && words[at + word.size()] == ' ')
      return true;
  return false;
}

bool letter(char byte) {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || byte == '_';
}

bool digit(char byte) {
  return byte >= '0' && byte <= '9';
}

// A template's file, as Name.cpp.in, by the name it is written for.
Syntax syntax_of(std::string_view path) {
  std::filesystem::path file(path);
  if (file.extension() == ".in")
    file.replace_extension();
  const std::string extension = file.extension().string();
  if (listed(coded, extension))
    return Syntax::code;
  if (listed(keyed, extension))
    return Syntax::keyed;
  if (extension == ".py")
    return Syntax::python;
  if (extension == ".cmake" || file.filename() == cmake_file)
    return Syntax::cmake;
  return extension == ".sh" ? Syntax::shell : Syntax::plain;
}

// A name in CamelCase, as types are named here (RC01) and classes in Python.
bool camel(std::string_view word) {
  return word.front() >= 'A' && word.front() <= 'Z' &&
         std::ranges::any_of(word, [](char byte) { return byte >= 'a' && byte <= 'z'; });
}

// A type by its word: C++'s and GLSL's own, a name in CamelCase, one ending in _t, and
// GLSL's vectors and matrices, as uvec2 or mat4x3.
bool typed(std::string_view word) {
  if (listed(types, word) || word.ends_with("_t") || camel(word))
    return true;
  if (std::string_view("iubd").find(word.front()) != std::string_view::npos)
    word.remove_prefix(1);
  return (word.starts_with("vec") || word.starts_with("mat")) && word.size() > 3 &&
         word[3] >= '2' && word[3] <= '4';
}

// A name in capitals, as CMake names its keywords and variables.
bool capitals(std::string_view word) {
  return std::ranges::none_of(word, [](char byte) { return byte >= 'a' && byte <= 'z'; });
}

// Past a quote's closing quote, a backslash keeping the next byte in; the row's end
// when it has none.
std::size_t quoted_end(std::string_view row, std::size_t at) {
  for (std::size_t end = at + 1; end < row.size(); ++end)
    if (row[end] == '\\')
      ++end;
    else if (row[end] == row[at])
      return end + 1;
  return row.size();
}

// Past a number's digits, letters, points and separators, as 0x1F, 1.5f, 1'000 or 2e-3.
std::size_t number_end(std::string_view row, std::size_t at) {
  std::size_t end = at + 1;
  while (end < row.size() &&
         (digit(row[end]) || letter(row[end]) || row[end] == '.' || row[end] == '\'' ||
          ((row[end] == '+' || row[end] == '-') &&
           exponents.find(row[end - 1]) != std::string_view::npos)))
    ++end;
  return end;
}

// Past a name's letters, digits and _, from a byte after its first; with dotted, its
// dots too, as a Python decorator's.
std::size_t name_end(std::string_view row, std::size_t from, bool dotted) {
  while (from < row.size() &&
         (letter(row[from]) || digit(row[from]) || (dotted && row[from] == '.')))
    ++from;
  return from;
}

// Past where a variable is named for its value: $name, $1 or $? in shell, ${name} in
// shell and CMake, and CMake's $ENV{name} and $<…>; at itself where none starts.
std::size_t variable_end(std::string_view row, std::size_t at, Syntax syntax) {
  if ((syntax != Syntax::shell && syntax != Syntax::cmake) || row[at] != '$' ||
      at + 1 == row.size())
    return at;
  const std::string_view rest = row.substr(at);
  if (rest.starts_with("${") || (syntax == Syntax::cmake && rest.starts_with("$ENV{")) ||
      (syntax == Syntax::cmake && rest.starts_with("$<"))) {
    const std::size_t close = row.find(rest[1] == '<' ? '>' : '}', at);
    return close == std::string_view::npos ? row.size() : close + 1;
  }
  if (syntax == Syntax::cmake)
    return at;
  if (letter(row[at + 1]))
    return name_end(row, at + 2, false);
  return shell_specials.find(row[at + 1]) != std::string_view::npos ? at + 2 : at;
}

// A word's role: a manifest's or an .ini's first of a row, its key or its section; a
// keyword of the language; a type, in code and in Python by the word, and in CMake a
// name in capitals; and in CMake a command, a name its ( follows.
std::uint32_t word_role(std::string_view word, Syntax syntax, bool first, bool called) {
  const bool keyword = (syntax == Syntax::keyed && first) ||
                       (syntax == Syntax::code && listed(keywords, word)) ||
                       (syntax == Syntax::python && listed(python_keywords, word)) ||
                       (syntax == Syntax::cmake && called) ||
                       (syntax == Syntax::shell && listed(shell_keywords, word));
  const bool python_type = listed(python_types, word) || camel(word);
  const bool type = (syntax == Syntax::code && typed(word)) ||
                    (syntax == Syntax::python && python_type) ||
                    (syntax == Syntax::cmake && capitals(word));
  if (keyword)
    return VP_VIEW::role("keyword");
  return type ? VP_VIEW::role("type") : VP_VIEW::role("text");
}

// What closes an open block, as long as what opens it.
std::string_view closing(Open open) {
  if (open == Open::comment)
    return "*/";
  return open == Open::double_quotes ? "\"\"\"" : "'''";
}

// What a run from here opens: a block comment in code, a string in three quotes in
// Python.
Open opening(std::string_view rest, Syntax syntax) {
  if (syntax == Syntax::code && rest.starts_with("/*"))
    return Open::comment;
  if (syntax == Syntax::python && rest.starts_with("\"\"\""))
    return Open::double_quotes;
  if (syntax == Syntax::python && rest.starts_with("'''"))
    return Open::single_quotes;
  return Open::none;
}

// Past what closes the open block, from where its insides start, and then nothing is
// open; the row's end, still open, where the row does not close it.
std::size_t closed_at(std::string_view row, std::size_t from, Open &open) {
  const std::string_view close = closing(open);
  const std::size_t end = row.find(close, from);
  if (end == std::string_view::npos)
    return row.size();
  open = Open::none;
  return end + close.size();
}

// Where a comment runs to the row's end: // in code; # in a manifest or an .ini; # in
// Python, CMake and shell where it starts a word, so a#b stays one.
bool comment_at(std::string_view row, std::size_t at, Syntax syntax) {
  if (syntax == Syntax::code)
    return row.substr(at).starts_with("//");
  return row[at] == '#' && (syntax == Syntax::keyed || at == 0 ||
                            (!letter(row[at - 1]) && !digit(row[at - 1])));
}

// Hands each run of the row in one role to each, as from, to and the role, in order and
// with every byte in one; returns what is open at the row's end. The comments, a
// directive, a Python decorator or a variable, quotes, numbers, and the words by their
// kind; in a manifest or an .ini, the first word of a row, its key or its section.
template <class Each>
Open scan(std::string_view row, Syntax syntax, Open open, Each each) {
  const std::size_t first = row.find_first_not_of(" \t");
  bool key = true; // no word of the row came yet
  std::size_t at = 0;
  const auto run = [&](std::size_t to, std::uint32_t role) {
    each(at, to, role);
    at = to;
  };
  while (at < row.size()) {
    const std::string_view rest = row.substr(at);
    const Open opened = open == Open::none ? opening(rest, syntax) : Open::none;
    const std::size_t variable = variable_end(row, at, syntax);
    if (syntax == Syntax::plain) {
      run(row.size(), VP_VIEW::role("text"));
    } else if (open != Open::none || opened != Open::none) {
      // Its insides start past what opens it, or at the row's start when a row before
      // opened it.
      const std::size_t inside = opened == Open::none ? at : at + closing(opened).size();
      if (opened != Open::none)
        open = opened;
      const std::uint32_t role =
          open == Open::comment ? VP_VIEW::role("comment") : VP_VIEW::role("string");
      run(closed_at(row, inside, open), role);
    } else if (comment_at(row, at, syntax)) {
      run(row.size(), VP_VIEW::role("comment"));
    } else if (syntax == Syntax::code && rest.front() == '#' && at == first) {
      run(row.size(), VP_VIEW::role("directive"));
    } else if (syntax == Syntax::python && rest.front() == '@' && at == first) {
      run(name_end(row, at + 1, true), VP_VIEW::role("directive"));
    } else if (variable != at) {
      run(variable, VP_VIEW::role("directive"));
    } else if (rest.front() == '"' || rest.front() == '\'') {
      run(quoted_end(row, at), VP_VIEW::role("string"));
    } else if (digit(rest.front()) ||
               (rest.size() > 1 && rest.front() == '.' && digit(rest[1]))) {
      run(number_end(row, at), VP_VIEW::role("number"));
    } else if (letter(rest.front())) {
      const std::size_t end = name_end(row, at + 1, false);
      const std::size_t next = row.find_first_not_of(" \t", end);
      const bool called = next != std::string_view::npos && row[next] == '(';
      run(end,
          word_role(row.substr(at, end - at), syntax, std::exchange(key, false), called));
    } else {
      run(at + 1, VP_VIEW::role("text"));
    }
  }
  return open;
}

} // namespace
