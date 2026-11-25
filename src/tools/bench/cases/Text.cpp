#include "runtime/Text.h"

#include "tools/bench/Bench.h"

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// runtime/Text.h: the text helpers every engine file reads with. The engine makes
// number_in for uint32_t, size_t and float, and joined for vectors and spans of strings
// and of views, an array of views and a transform_view of usages, all the one loop.

namespace VP::bench {

namespace {

// Whether part lies inside text, as a view into it does.
bool inside(std::string_view part, std::string_view text) {
  return part.empty() || (part.data() >= text.data() &&
                          part.data() + part.size() <= text.data() + text.size());
}

// lines_in as its comment says it: the text split at each \n, one \r taken off the end of
// each line, the lines that hold nothing left out.
std::vector<std::string_view> lines_as_said(std::string_view text) {
  std::vector<std::string_view> lines;
  while (!text.empty()) {
    const std::size_t end = text.find('\n');
    std::string_view line = text.substr(0, end);
    if (line.ends_with('\r'))
      line.remove_suffix(1);
    if (!line.empty())
      lines.push_back(line);
    text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
  }
  return lines;
}

std::vector<std::string_view> views_of(const std::vector<std::string> &names) {
  return {names.begin(), names.end()};
}

} // namespace

void cases(Bench &bench) {
  bench.sweep("Text.h/trim", trim);
  bench.holds("Text.h/trim", "trim twice is trim once", [](std::string_view text) {
    return trim(trim(text)) == trim(text);
  });
  bench.holds("Text.h/trim", "no blank at either end", [](std::string_view text) {
    const std::string_view kept = trim(text);
    return kept.empty() || (blanks.find(kept.front()) == std::string_view::npos &&
                            blanks.find(kept.back()) == std::string_view::npos);
  });
  bench.holds("Text.h/trim", "a view into the text", [](std::string_view text) {
    return inside(trim(text), text);
  });

  bench.sweep("Text.h/number_in<uint32_t>", number_in<std::uint32_t>, domain::Word{});
  bench.sweep("Text.h/number_in<size_t>", number_in<std::size_t>, domain::Word{});
  bench.sweep("Text.h/number_in<float>", number_in<float>, domain::Word{});
  bench.holds("Text.h/number_in<uint32_t>",
              "a number's shortest text reads back as it",
              [](std::uint32_t value) {
                return number_in<std::uint32_t>(number_text(value)) == value;
              });
  bench.holds("Text.h/number_in<size_t>",
              "a number's shortest text reads back as it",
              [](std::size_t value) {
                return number_in<std::size_t>(number_text(value)) == value;
              });
  bench.holds("Text.h/number_in<float>",
              "a finite float's shortest text reads back as it",
              [](float value) {
                return !std::isfinite(value) ||
                       number_in<float>(number_text(value)) == value;
              });

  // Each instantiation is the one loop over another range, so the others are held to the
  // vector's rows.
  bench.sweep("Text.h/joined<vector<string>>", joined<std::vector<std::string>>);
  bench.holds("Text.h/joined<span<const string>>",
              "as joined<vector<string>>",
              [](const std::vector<std::string> &names, std::string_view between) {
                return joined(std::span<const std::string>(names), between) ==
                       joined(names, between);
              });
  bench.holds("Text.h/joined<vector<string_view>>",
              "as joined<vector<string>>",
              [](const std::vector<std::string> &names, std::string_view between) {
                return joined(views_of(names), between) == joined(names, between);
              });
  bench.holds("Text.h/joined<span<const string_view>>",
              "as joined<vector<string>>",
              [](const std::vector<std::string> &names, std::string_view between) {
                const std::vector<std::string_view> views = views_of(names);
                return joined(std::span<const std::string_view>(views), between) ==
                       joined(names, between);
              });
  // A between that shares no letter with a name can only be found where joined put it.
  bench.holds("Text.h/joined<vector<string>>",
              "the names come back, split where between is",
              [](const std::vector<std::string> &names, std::string_view between) {
                if (between.empty() || names.empty() ||
                    std::ranges::any_of(names, [&](const std::string &name) {
                      return name.find_first_of(between) != std::string::npos;
                    }))
                  return true;
                std::vector<std::string_view> parts;
                const std::string whole = joined(names, between);
                std::string_view text = whole;
                for (std::size_t at = text.find(between); at != std::string_view::npos;
                     at = text.find(between)) {
                  parts.push_back(text.substr(0, at));
                  text.remove_prefix(at + between.size());
                }
                parts.push_back(text);
                return std::ranges::equal(parts, names);
              });

  bench.sweep("Text.h/lines_in", lines_in, domain::FileText{});
  bench.holds(
      "Text.h/lines_in",
      "the lines its comment says, in order",
      [](std::string_view text) { return lines_in(text) == lines_as_said(text); },
      domain::FileText{});
  bench.holds(
      "Text.h/lines_in",
      "each line a view into the text, holding no \\n",
      [](std::string_view text) {
        return std::ranges::all_of(lines_in(text), [&](std::string_view line) {
          return inside(line, text) && !line.empty() &&
                 line.find('\n') == std::string_view::npos;
        });
      },
      domain::FileText{});
}

} // namespace VP::bench
