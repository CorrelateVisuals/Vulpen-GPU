#include "shared/Ini.h"

#include "tools/bench/Bench.h"

#include <algorithm>
#include <cmath>
#include <array>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The library's shared/Ini.h: how every part's .ini reads, the palette's theme, the
// keys' keymap, the font's and the graph's kept files, so one rule holds for all of them.

namespace VP::bench {

template <> struct Shown<::Row> {
  static std::string text(const ::Row &row, Names &) {
    return std::format("{}: {}", row.line, escaped(row.text));
  }
};

namespace {

bool inside(std::string_view part, std::string_view text) {
  return part.empty() || (part.data() >= text.data() &&
                          part.data() + part.size() <= text.data() + text.size());
}

bool blank_at_an_end(std::string_view text) {
  return !text.empty() && (ini_blanks.find(text.front()) != std::string_view::npos ||
                           ini_blanks.find(text.back()) != std::string_view::npos);
}

// Usages as commands declare them: words, then placeholders, an option in brackets.
constexpr std::array usages{VP::Usage{"quit", ""},
                            VP::Usage{"quit force", ""},
                            VP::Usage{"view close [<view>]", ""},
                            VP::Usage{"param set <node> <key> <value>", ""},
                            VP::Usage{"window fullscreen on", ""},
                            VP::Usage{"<value>", ""}};

} // namespace

void cases(Bench &bench) {
  bench.sweep("Ini.h/trim", ::trim);
  bench.holds("Ini.h/trim", "trim twice is trim once", [](std::string_view text) {
    return ::trim(::trim(text)) == ::trim(text);
  });
  bench.holds("Ini.h/trim",
              "no blank at either end, a view into the text",
              [](std::string_view text) {
                const std::string_view kept = ::trim(text);
                return !blank_at_an_end(kept) && inside(kept, text);
              });

  bench.sweep("Ini.h/rows_of", ::rows_of, domain::FileText{});
  bench.holds(
      "Ini.h/rows_of",
      "its rows hold no comment, no line break and no blank at an end, their lines "
      "rising",
      [](std::string_view text) {
        std::size_t line = 0;
        return std::ranges::all_of(::rows_of(text), [&](const ::Row &row) {
          const bool rises = row.line > line;
          line = row.line;
          return rises && !row.text.empty() && inside(row.text, text) &&
                 row.text.find_first_of("#\n") == std::string_view::npos &&
                 !blank_at_an_end(row.text);
        });
      },
      domain::FileText{});

  bench.sweep("Ini.h/key_value", key_value);
  bench.holds("Ini.h/key_value",
              "split at the first =, either side trimmed",
              [](std::string_view row) {
                const auto pair = key_value(row);
                const std::size_t equals = row.find('=');
                if (!pair)
                  return equals == std::string_view::npos;
                return pair->first.find('=') == std::string_view::npos &&
                       pair->first == ::trim(row.substr(0, equals)) &&
                       pair->second == ::trim(row.substr(equals + 1));
              });

  bench.sweep(
      "Ini.h/registered",
      [](std::string_view line) { return registered(line, usages); },
      domain::Word{});
  bench.check("Ini.h/registered", [](Rows &rows) {
    for (const std::string_view line : {"quit",
                                        "quit force",
                                        "quit now",
                                        "quits",
                                        "view close",
                                        "view close ide",
                                        "view",
                                        "view closed",
                                        "param set a b 1",
                                        "param",
                                        "window fullscreen on",
                                        "window fullscreen off",
                                        "window",
                                        "",
                                        "x"})
      rows.row(std::format("{} -> {}", escaped(line), registered(line, usages)));
    rows.row(std::format("quit with none registered -> {}",
                         registered("quit", std::span<const VP::Usage>{})));
  });

  bench.sweep("Ini.h/numbers_of", numbers_of, domain::Word{});
  bench.check("Ini.h/numbers_of", [](Rows &rows) {
    for (const std::string_view value : {"1 2 3",
                                         " 0.5\t1e3 ",
                                         "",
                                         "1,2",
                                         "1 x",
                                         "+1",
                                         "-1",
                                         "nan",
                                         "inf",
                                         "1e40",
                                         "0x10",
                                         ".5",
                                         "1.",
                                         "1 2\r"})
      rows.row(
          std::format("{} -> {}", escaped(value), show(numbers_of(value), rows.names())));
  });
  bench.holds("Ini.h/numbers_of",
              "finite numbers written out read back as they were",
              [](const std::vector<float> &numbers) {
                if (!std::ranges::all_of(
                        numbers, [](float number) { return std::isfinite(number); }))
                  return true;
                std::string text;
                for (const float number : numbers)
                  text.append(text.empty() ? "" : " ").append(number_text(number));
                return numbers_of(text) == numbers;
              });
}

} // namespace VP::bench
