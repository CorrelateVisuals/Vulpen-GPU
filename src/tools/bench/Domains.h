#pragma once

#include "tools/bench/Shown.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The bench's inputs (Bench.h): those of a type, which every parameter of the type
// gets, and domains, meanings narrower than a type, which a case gives a parameter: a
// std::string_view may be a word of a line, a file's whole text, a name or a node's path,
// each with edges of its own. A domain only one case uses stays in that case.

namespace VP::bench {

// Seeded per function from its id, so its random rows stay when others are added. The
// engine's raw output is the same on every standard library; a distribution's mapping is
// not, so the bench maps it itself (C10).
class Random {
public:
  explicit Random(std::uint64_t seed) : _engine(seed) {}
  std::uint64_t bits() {
    return _engine();
  }
  std::uint64_t below(std::uint64_t count) {
    return count == 0 ? 0 : _engine() % count;
  }

private:
  std::mt19937_64 _engine;
};

// A type's inputs: its edges, picked by hand, the first a typical value, which a row
// holds while it tries another parameter's edges; and random values. Gen of a vector, an
// optional, a pair or a glm vector is built from Gen of what it holds.
template <class T> struct Gen {
  static_assert(sizeof(T) == 0,
                "no Gen<T> for this parameter's type: add one to the "
                "bench, or give the parameter a domain in the case");
};

template <> struct Gen<bool> {
  using type = bool;
  static std::vector<bool> edges() {
    return {true, false};
  }
  static bool random(Random &random) {
    return random.below(2) == 1;
  }
};

template <> struct Gen<char> {
  using type = char;
  static std::vector<char> edges() {
    return {'a',
            ' ',
            '\0',
            '\t',
            '\n',
            '\r',
            '.',
            ',',
            '=',
            '"',
            '\\',
            '\x7f',
            '\xc3',
            '\xff'};
  }
  static char random(Random &random) {
    return static_cast<char>(random.bits());
  }
};

template <class T>
  requires std::integral<T>
struct Gen<T> {
  using type = T;
  static std::vector<T> edges() {
    using limits = std::numeric_limits<T>;
    std::vector<T> all{1, 0, 2, 7, limits::max(), static_cast<T>(limits::max() - 1)};
    if constexpr (std::is_signed_v<T>)
      all.insert(all.end(), {-1, limits::min(), static_cast<T>(limits::min() + 1)});
    if constexpr (sizeof(T) > 1)
      all.insert(all.end(), {T{255}, T{256}});
    if constexpr (sizeof(T) > 2)
      all.insert(all.end(), {T{65535}, T{65536}});
    if constexpr (sizeof(T) > 4)
      all.push_back(static_cast<T>(std::uint64_t{1} << 32));
    return all;
  }
  static T random(Random &random) {
    switch (random.below(3)) {
      case 0:
        return static_cast<T>(random.below(16));
      case 1:
        return static_cast<T>(random.bits());
      default:
        return static_cast<T>(std::numeric_limits<T>::max() -
                              static_cast<T>(random.below(16)));
    }
  }
};

template <class T>
  requires std::floating_point<T>
struct Gen<T> {
  using type = T;
  static std::vector<T> edges() {
    using limits = std::numeric_limits<T>;
    return {T(1),
            T(0),
            T(-0.0),
            T(-1),
            T(0.5),
            T(0.1),
            T(2),
            T(1e6),
            limits::min(),
            limits::denorm_min(),
            limits::max(),
            limits::lowest(),
            limits::epsilon(),
            limits::infinity(),
            -limits::infinity(),
            limits::quiet_NaN()};
  }
  static T random(Random &random) {
    using Bits = std::conditional_t<sizeof(T) == 4, std::uint32_t, std::uint64_t>;
    switch (random.below(3)) {
      case 0: // a grid from -10 to 10, a hundredth apart
        return static_cast<T>(static_cast<double>(random.below(2001)) / 100.0 - 10.0);
      case 1: // any bits: a nan, an infinity, a subnormal
        return std::bit_cast<T>(static_cast<Bits>(random.bits()));
      default: // a power of two
        return static_cast<T>(std::ldexp(1.0, static_cast<int>(random.below(80)) - 40));
    }
  }
};

template <> struct Gen<std::string> {
  using type = std::string;
  static std::vector<std::string> edges();
  static std::string random(Random &random);
};

template <> struct Gen<std::filesystem::path> {
  using type = std::filesystem::path;
  static std::vector<std::filesystem::path> edges();
  static std::filesystem::path random(Random &random);
};

template <class T> struct Gen<std::vector<T>> {
  using type = std::vector<T>;
  static std::vector<type> edges() {
    const std::vector<T> items = Gen<T>::edges();
    const T &first = items.front();
    const T &second = items[std::min<std::size_t>(1, items.size() - 1)];
    return {{first}, {}, {first, second}, {second, first}, {first, first}, items};
  }
  static type random(Random &random) {
    type items(random.below(6));
    for (T &item : items)
      item = Gen<T>::random(random);
    return items;
  }
};

template <class T> struct Gen<std::optional<T>> {
  using type = std::optional<T>;
  static std::vector<type> edges() {
    std::vector<type> all{Gen<T>::edges().front(), std::nullopt};
    for (const T &item : Gen<T>::edges())
      all.emplace_back(item);
    return all;
  }
  static type random(Random &random) {
    if (random.below(4) == 0)
      return std::nullopt;
    return Gen<T>::random(random);
  }
};

template <class A, class B> struct Gen<std::pair<A, B>> {
  using type = std::pair<A, B>;
  static std::vector<type> edges() {
    std::vector<type> all;
    for (const A &first : Gen<A>::edges())
      all.emplace_back(first, Gen<B>::edges().front());
    for (const B &second : Gen<B>::edges())
      all.emplace_back(Gen<A>::edges().front(), second);
    return all;
  }
  static type random(Random &random) {
    A first = Gen<A>::random(random);
    return {std::move(first), Gen<B>::random(random)};
  }
};

// Each component an edge of its scalar in turn, the others 1, and the whole vector's own
// edges: all zeros, and 1, 2, 3 ... so a swap of components shows.
template <glm::length_t L, class T, glm::qualifier Q> struct Gen<glm::vec<L, T, Q>> {
  using type = glm::vec<L, T, Q>;
  static std::vector<type> edges() {
    std::vector<type> all{type(T(1)), type(T(0))};
    type counted{};
    for (glm::length_t index = 0; index < L; ++index)
      counted[index] = T(index + 1);
    all.push_back(counted);
    for (glm::length_t index = 0; index < L; ++index)
      for (const T &edge : Gen<T>::edges()) {
        type one(T(1));
        one[index] = edge;
        all.push_back(one);
      }
    return all;
  }
  static type random(Random &random) {
    type value{};
    for (glm::length_t index = 0; index < L; ++index)
      value[index] = Gen<T>::random(random);
    return value;
  }
};

namespace domain {
// The parameter's type's own inputs, where a later parameter has a domain.
struct Type {};
// One value, sevens, which an out parameter starts as, so a write before a refusal shows.
struct Unset {};
} // namespace domain

template <class T> struct Sevens {
  using type = T;
  static std::vector<T> edges() {
    return {seven()};
  }
  static T random(Random &) {
    return seven();
  }
  static T seven() {
    if constexpr (std::is_constructible_v<T, int>)
      return T(7);
    else
      return T{};
  }
};

// Where a parameter's inputs come from: its domain's Gen, else its type's.
template <class Domain, class Held> struct Source {
  using gen = Gen<Domain>;
};
template <class Held> struct Source<domain::Type, Held> {
  using gen = Gen<Held>;
};
template <class Held> struct Source<domain::Unset, Held> {
  using gen = Sevens<Held>;
};

namespace domain {
// One word of a manifest or a typed line, as a number or a name is read from it.
struct Word {};
// A whole file's text: its lines, their ends as Windows and Unix write them, and none.
struct FileText {};
// A name, as a node, a view, a port or a folder is named.
struct Name {};
// Names joined by dots: a node within the nodes it is inside.
struct NodePath {};
} // namespace domain

namespace words {
// Letters a random text of each domain is made of.
inline constexpr std::string_view number_letters = "0123456789+-.,eEx_ab ";
inline constexpr std::string_view name_letters = "aZ09_-. :/";
inline constexpr std::string_view line_letters = "ab =#\t\r\n";

inline std::string
drawn(Random &random, std::string_view letters, std::uint64_t longest) {
  std::string text(random.below(longest + 1), ' ');
  for (char &letter : text)
    letter = letters[random.below(letters.size())];
  return text;
}
} // namespace words

template <> struct Gen<domain::Word> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"1",
            "",
            "0",
            "-0",
            "-1",
            "+1",
            " 1",
            "1 ",
            "01",
            "1.5",
            ".5",
            "5.",
            "1e3",
            "1e",
            "0x10",
            "nan",
            "inf",
            "-inf",
            "4294967295",
            "4294967296",
            "18446744073709551615",
            "18446744073709551616",
            "-2147483648",
            "1,2",
            "1_0",
            "1\r",
            "1\n",
            "a",
            "a=b",
            "=",
            "\xd9\xa1",
            "\xef\xbc\x91",
            "3.4028235e38",
            "3.4028236e38",
            "1e-40",
            "1e-50"};
  }
  static std::string random(Random &random) {
    return words::drawn(random, words::number_letters, 8);
  }
};

template <> struct Gen<domain::FileText> {
  using type = std::string;
  static std::vector<std::string> edges() {
    using namespace std::string_literals;
    return {"a\n",
            "",
            "\n",
            "a",
            "a\nb",
            "a\r\nb\r\n",
            "\r\n",
            "\r",
            "a\r",
            "a\r\r\n",
            "\n\n",
            "a\n\nb",
            " \n",
            "  a  \n",
            "\ta\n",
            "\xef\xbb\xbf"
            "a\n",
            "a\0b\n"s,
            "a\n\r\nb",
            "\r\r\n",
            std::string(300, 'a') + "\n"};
  }
  static std::string random(Random &random) {
    return words::drawn(random, words::line_letters, 16);
  }
};

template <> struct Gen<domain::Name> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"a",
            "",
            "a-b",
            "a_b",
            "A9",
            "a.b",
            "a b",
            "root",
            "-",
            "_",
            ".",
            "a/b",
            "\xc3\xa9",
            "a:b",
            "1",
            "a\t",
            "a\r",
            std::string(300, 'a')};
  }
  static std::string random(Random &random) {
    return words::drawn(random, words::name_letters, 6);
  }
};

template <> struct Gen<domain::NodePath> {
  using type = std::string;
  static std::vector<std::string> edges() {
    return {"a",
            "a.b",
            "a..b",
            ".a",
            "a.",
            "root",
            "a.b.c",
            "",
            ".",
            "a b",
            "a.root",
            "root.a",
            "a.b ",
            "\xc3\xa9.a",
            "a/b",
            std::string(300, 'a') + ".b"};
  }
  static std::string random(Random &random) {
    return words::drawn(random, words::name_letters, 8);
  }
};

} // namespace VP::bench
