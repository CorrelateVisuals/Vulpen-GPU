#include "shared/Params.h"

#include "tools/bench/Bench.h"

#include <format>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>

// The library's shared/Params.h: a param that takes one of a closed set of words, which
// ten parts check, so each refusal names its words alike. Its words come in braces, so
// the rows try the sets its parts give: one word, two, and three.

namespace VP::bench {

namespace {

std::string checked(std::string_view key,
                    std::string_view value,
                    std::initializer_list<std::string_view> words) {
  try {
    check_word(key, value, words);
    return "taken";
  } catch (...) {
    return thrown();
  }
}

} // namespace

void cases(Bench &bench) {
  bench.sweep(
      "Params.h/check_word",
      [](std::string_view key, std::string_view value) {
        return std::format("{} / {} / {}",
                           checked(key, value, {"on"}),
                           checked(key, value, {"x", "y"}),
                           checked(key, value, {"left", "right", "middle"}));
      },
      domain::Name{},
      domain::Word{});
  bench.check("Params.h/check_word", [](Rows &rows) {
    for (const std::string_view value : {"x", "y", "", "X", "x ", " x", "z", "x,y"})
      rows.row(std::format(
          "axis = {} of x, y -> {}", escaped(value), checked("axis", value, {"x", "y"})));
    for (const std::string_view value : {"left", "right", "middle", "mid"})
      rows.row(std::format("button = {} of left, right, middle -> {}",
                           escaped(value),
                           checked("button", value, {"left", "right", "middle"})));
  });
  bench.holds(
      "Params.h/check_word",
      "a refusal names the key, the value and every word",
      [](std::string_view key, std::string_view value) {
        try {
          check_word(key, value, {"left", "right", "middle"});
          return value.empty() || value == "left" || value == "right" ||
                 value == "middle";
        } catch (const std::runtime_error &error) {
          return error.what() ==
                 std::format("param {} = {}: left, right or middle", key, value);
        }
      },
      domain::Name{},
      domain::Word{});
}

} // namespace VP::bench
