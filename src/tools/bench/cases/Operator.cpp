#include "runtime/Operator.h"

#include "tools/bench/Bench.h"
#include "tools/bench/Fakes.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// runtime/Operator.h: what a node's C++ reads its params with, for each of the twelve
// types the engine makes parse_param for, each value starting as sevens so a write before
// a refusal shows; and Bind::param, as a node calls it, through a bind that records what
// the node asked.

namespace VP::bench {

namespace domain {
// A param's text as T would read it: what every type shares, then T's own; a vector's
// built from its scalar's, a component spoiled after the first parsed.
template <class T> struct ParamText {};
} // namespace domain

template <class T> struct Gen<domain::ParamText<T>> {
  using type = std::string;
  static std::vector<std::string> shared() {
    using namespace std::string_literals;
    return {"1",   "",     " ",   "0",  "-0",  "-1",       "+1",     " 1",
            "1 ",  "1\n",  "1\r", "01", "1_0", "0x10",     "1e3",    "nan",
            "inf", "-inf", "1,2", ",",  "1.5", "\xd9\xa1", "1\0002"s};
  }
  static std::vector<std::string> own() {
    if constexpr (std::is_same_v<T, float>)
      return {"1e40",
              "-1e40",
              "1e-40",
              "1e-50",
              "3.4028235e38",
              "3.4028236e38",
              ".5",
              "5.",
              "0.1",
              "1e",
              "e1"};
    else if constexpr (std::is_same_v<T, std::int32_t>)
      return {"2147483647", "2147483648", "-2147483648", "-2147483649", "1.0", "1e0"};
    else if constexpr (std::is_same_v<T, std::uint32_t>)
      return {"4294967295", "4294967296", "1.0"};
    else
      return {};
  }
  static std::vector<std::string> edges() {
    if constexpr (requires { T::length(); }) {
      using Scalar = typename T::value_type;
      std::string whole;
      for (glm::length_t index = 0; index < T::length(); ++index)
        whole += (index ? "," : "") + std::to_string(index + 1);
      std::string spoiled = whole;
      spoiled[2] = 'x';
      std::vector<std::string> all{whole,
                                   "",
                                   ",",
                                   "1",
                                   "1,",
                                   ",1",
                                   "1, 2",
                                   "1 ,2",
                                   "1,,2",
                                   whole + ",",
                                   whole + ",9",
                                   spoiled};
      for (const std::string &scalar : Gen<domain::ParamText<Scalar>>::edges())
        if (scalar.find(',') == std::string::npos) {
          std::string row;
          for (glm::length_t index = 0; index < T::length(); ++index)
            row += (index ? "," : "") +
                   (index + 1 == T::length() ? scalar : std::string("1"));
          all.push_back(row);
        }
      return all;
    } else {
      std::vector<std::string> all = shared();
      for (std::string &each : own())
        all.push_back(std::move(each));
      return all;
    }
  }
  static std::string random(Random &random) {
    return words::drawn(random, words::number_letters, 10);
  }
};

namespace {

// Bind::param as a node calls it: the value it reads, and what the bind was asked.
template <class T> std::string param_read(std::string_view text) {
  FakeBind bind;
  bind.params = {{"x", std::string(text)}};
  const T value = bind.param<T>("x");
  Names names;
  return show(value, names) + "; asked " + show(bind.asked, names);
}

template <class T> void sweep_param(Bench &bench, const std::string &name) {
  bench.sweep(std::format("Operator.h/parse_param<{}>", name),
              parse_param<T>,
              domain::ParamText<T>{},
              domain::Unset{});
}

} // namespace

void cases(Bench &bench) {
  sweep_param<float>(bench, "float");
  sweep_param<std::int32_t>(bench, "int");
  sweep_param<std::uint32_t>(bench, "uint");
  sweep_param<glm::vec2>(bench, "vec2");
  sweep_param<glm::vec3>(bench, "vec3");
  sweep_param<glm::vec4>(bench, "vec4");
  sweep_param<glm::ivec2>(bench, "ivec2");
  sweep_param<glm::ivec3>(bench, "ivec3");
  sweep_param<glm::ivec4>(bench, "ivec4");
  sweep_param<glm::uvec2>(bench, "uvec2");
  sweep_param<glm::uvec3>(bench, "uvec3");
  sweep_param<glm::uvec4>(bench, "uvec4");

  bench.holds(
      "Operator.h/parse_param<float>",
      "a refused text leaves the value as it was",
      [](std::string_view text) {
        float value = 7;
        return parse_param(text, value) || value == 7;
      },
      domain::ParamText<float>{});
  bench.holds(
      "Operator.h/parse_param<vec3>",
      "a refused text leaves the value as it was",
      [](std::string_view text) {
        glm::vec3 value(7);
        return parse_param(text, value) || value == glm::vec3(7);
      },
      domain::ParamText<glm::vec3>{});

  bench.sweep(
      "Operator.h/Bind::param<float>", param_read<float>, domain::ParamText<float>{});
  bench.sweep("Operator.h/Bind::param<uint>",
              param_read<std::uint32_t>,
              domain::ParamText<std::uint32_t>{});
  bench.sweep("Operator.h/Bind::param<vec3>",
              param_read<glm::vec3>,
              domain::ParamText<glm::vec3>{});
  bench.sweep("Operator.h/Bind::param<string>", param_read<std::string>);
}

} // namespace VP::bench
