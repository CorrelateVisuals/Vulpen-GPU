#include "tools/bench/Bench.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <numeric>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

// tools/bench/Shown.h: the text a value shows as in a row, which every record of
// the review goes through. A value shown as another's text would hide a change in every
// record holding it, so these claims hold the text to the value: a number reads back as
// its bits, a text escapes to what only it gives, a long list keeps what it leaves out
// as a hash, and a handle keeps its number in a row.

namespace VP::bench {

namespace {

constexpr std::size_t long_list = 20; // past shown_items, so its middle is left out

template <class T> bool reads_back(T value) {
  const std::string text = number_text(value);
  if (std::isnan(value))
    return text == (std::signbit(value) ? "-nan" : "nan");
  T back{};
  const auto [end, failed] =
      std::from_chars(text.data(), text.data() + text.size(), back);
  using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
  return failed == std::errc{} && end == text.data() + text.size() &&
         std::bit_cast<Bits>(back) == std::bit_cast<Bits>(value);
}

std::string list_of(const std::vector<int> &items) {
  Names names;
  return list_text(items, names);
}

} // namespace

void cases(Bench &bench) {
  bench.sweep("Shown.h/number_text<float>", number_text<float>);
  bench.holds("Shown.h/number_text<float>",
              "reads back as the same bits, nan as nan",
              reads_back<float>);
  bench.holds("Shown.h/number_text<double>",
              "reads back as the same bits, nan as nan",
              reads_back<double>);

  bench.sweep("Shown.h/escaped", escaped);
  bench.holds("Shown.h/escaped",
              "leaves no byte a terminal obeys, and none past ASCII",
              [](std::string_view text) {
                return std::ranges::all_of(escaped(text), [](char letter) {
                  return letter >= ' ' && letter < '\x7f';
                });
              });
  bench.holds("Shown.h/escaped",
              "two texts escape alike only when they are one",
              [](const std::string &one, const std::string &other) {
                return (escaped(one) == escaped(other)) == (one == other);
              });

  bench.holds("Shown.h/list_text",
              "a long list with any one item changed shows otherwise",
              [](std::size_t at, int value) {
                std::vector<int> items(long_list);
                for (std::size_t index = 0; index < items.size(); ++index)
                  items[index] = static_cast<int>(index);
                std::vector<int> changed = items;
                changed[at % long_list] =
                    value == changed[at % long_list] ? value + 1 : value;
                return list_of(items) != list_of(changed);
              });
  // Either side of shown_items, where a list starts leaving its middle out.
  bench.check("Shown.h/list_text", [](Rows &rows) {
    for (const std::size_t count : {shown_items - 1, shown_items, shown_items + 1}) {
      std::vector<int> items(count);
      std::iota(items.begin(), items.end(), 0);
      rows.row(std::format("{} items -> {}", count, list_of(items)));
    }
  });

  bench.holds("Shown.h/Names",
              "a handle keeps its number in a row, another gets another",
              [](int one, int other) {
                Names names;
                const std::size_t first = names.number(&one);
                return names.number(&one) == first && names.number(&other) != first;
              });
}

} // namespace VP::bench
