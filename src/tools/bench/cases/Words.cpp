#include "shared/Words.h"

#include "tools/bench/Bench.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// The library's shared/Words.h: how the editor and the terminal take a text's rows and
// its words, so both select and delete alike, and how parts name a class, a copy and the
// shader files a folder lacks.

namespace VP::bench {

template <> struct Shown<::Run> {
  static std::string text(::Run run, Names &) {
    return run == ::Run::word ? "word" : run == ::Run::blank ? "blank" : "other";
  }
};

namespace {

std::string joined_rows(const std::vector<std::string> &rows) {
  std::string text;
  for (const std::string &row : rows)
    text.append(&row == rows.data() ? "" : "\n").append(row);
  return text;
}

} // namespace

void cases(Bench &bench) {
  bench.sweep("Words.h/rows_of_text", rows_of_text, domain::FileText{});
  bench.holds(
      "Words.h/rows_of_text",
      "joined by line breaks, its rows are the text",
      [](std::string_view text) { return joined_rows(rows_of_text(text)) == text; },
      domain::FileText{});

  bench.sweep("Words.h/run_of", run_of);

  bench.sweep("Words.h/word_at", word_at);
  bench.holds("Words.h/word_at",
              "the run holds the byte pressed, or the last, and only bytes of its kind, "
              "and ends where the kind does",
              [](std::string_view row, std::size_t column) {
                const auto [from, to] = word_at(row, column);
                if (row.empty())
                  return from == 0 && to == 0;
                const std::size_t pressed = std::min(column, row.size() - 1);
                const ::Run run = run_of(row[pressed]);
                return from <= pressed && pressed < to && to <= row.size() &&
                       std::all_of(row.begin() + static_cast<std::ptrdiff_t>(from),
                                   row.begin() + static_cast<std::ptrdiff_t>(to),
                                   [&](char byte) { return run_of(byte) == run; }) &&
                       (from == 0 || run_of(row[from - 1]) != run) &&
                       (to == row.size() || run_of(row[to]) != run);
              });

  bench.sweep("Words.h/word_before", word_before);
  bench.holds("Words.h/word_before",
              "back from the column, and the bytes passed are blanks, then others, then "
              "a word",
              [](std::string_view row, std::size_t column) {
                const std::size_t end = std::min(column, row.size());
                const std::size_t start = word_before(row, column);
                const auto stage = [](::Run run) {
                  return run == ::Run::blank ? 0 : run == ::Run::other ? 1 : 2;
                };
                int reached = 0;
                for (std::size_t at = end; at > start; --at) {
                  const int now = stage(run_of(row[at - 1]));
                  if (now < reached)
                    return false;
                  reached = now;
                }
                return start <= end;
              });
  bench.sweep("Words.h/word_after", word_after);
  bench.holds("Words.h/word_after",
              "on from the column, never past the row's end",
              [](std::string_view row, std::size_t column) {
                const std::size_t end = word_after(row, column);
                return end >= std::min(column, row.size()) && end <= row.size();
              });
  bench.holds("Words.h/word_after",
              "as word_before on the row reversed",
              [](std::string_view row, std::size_t column) {
                const std::string reversed(row.rbegin(), row.rend());
                const std::size_t at = std::min(column, row.size());
                return row.size() - word_after(row, at) ==
                       word_before(reversed, row.size() - at);
              });

  bench.sweep("Words.h/class_of", class_of, domain::Name{});
  bench.holds(
      "Words.h/class_of",
      "the name without its dashes, each letter after one upper case",
      [](std::string_view name) {
        std::string undashed;
        std::ranges::copy_if(name, std::back_inserter(undashed), [](char letter) {
          return letter != '-';
        });
        const std::string type = class_of(name);
        return type.size() == undashed.size() &&
               std::ranges::equal(type, undashed, [](char made, char given) {
                 return made == given || (given >= 'a' && given <= 'z' &&
                                          made == static_cast<char>(given - 'a' + 'A'));
               });
      },
      domain::Name{});

  bench.sweep("Words.h/numbered", numbered, domain::Name{});
  bench.check("Words.h/numbered", [](Rows &rows) {
    for (const std::string_view name : {"wave",
                                        "wave2",
                                        "wave9",
                                        "wave10",
                                        "wave0",
                                        "wave01",
                                        "2",
                                        "",
                                        "w2a",
                                        "wave18446744073709551615",
                                        "wave18446744073709551616"}) {
      const auto [stem, number] = numbered(name);
      rows.row(std::format("a copy of {} -> {}{}", escaped(name), stem, number));
    }
  });
  bench.holds(
      "Words.h/numbered",
      "the copy's name is not the name",
      [](std::string_view name) {
        const auto [stem, number] = numbered(name);
        return std::format("{}{}", stem, number) != name;
      },
      domain::Name{});

  bench.sweep("Words.h/lacking", lacking);
  bench.check("Words.h/lacking", [](Rows &rows) {
    const std::vector<std::vector<std::string>> folders{{},
                                                        {"Name.vert"},
                                                        {"Name.frag"},
                                                        {"Name.vert", "Name.frag"},
                                                        {"Name.comp"},
                                                        {"Name.cpp", "Name.h"},
                                                        {"a.vert", "b.frag"},
                                                        {"Name.geom"},
                                                        {"Name.geom", "Name.vert"},
                                                        {"Name.cpp", "Name.vert"},
                                                        {".vert"},
                                                        {"a.b.vert"},
                                                        {"vert"},
                                                        {"Name.tesc"},
                                                        {"Name.comp", "Name.vert"}};
    for (const std::vector<std::string> &files : folders)
      rows.row(std::format(
          "{} lacks {}", show(files, rows.names()), show(lacking(files), rows.names())));
  });
}

} // namespace VP::bench
