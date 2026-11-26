#pragma once

#include "tools/bench/Domains.h"
#include "tools/bench/Shown.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// The bench: each engine function run on its own, with inputs its parameters'
// types make, and what each call did written as a row of text, so the same behaviour
// gives the same bytes in every run, build and context. A case file per engine file
// registers its functions (cases(), at the bottom); src/tools/bench/bench.py builds a
// bench per case, runs each function's rows in every context and compares them with the
// records committed beside it, so a change of behaviour shows as a diff a person reads
// (A00), whatever the code's shape becomes.
//
// Standard C++ and the engine's own libraries only, so it builds wherever the engine does
// (RA01, C10); what differs between processes, bench.py sets.

namespace VP::bench {

// What the bench keeps of a parameter between rows, which outlives each call: a view's
// text, a span's items.
template <class T> struct Held {
  using type = T;
};
template <> struct Held<std::string_view> {
  using type = std::string;
};
template <class T, std::size_t N> struct Held<std::span<T, N>> {
  using type = std::vector<std::remove_const_t<T>>;
};
template <class Argument>
using held_t = typename Held<std::remove_cvref_t<Argument>>::type;

// A parameter a call may change, shown again after it.
template <class Argument>
inline constexpr bool changed = std::is_lvalue_reference_v<Argument> &&
                                !std::is_const_v<std::remove_reference_t<Argument>>;

// A parameter of the type the bench holds is passed the held value itself; one of
// another type, as a span over held items, a value made from it for the call.
template <class Argument> decltype(auto) pass(held_t<Argument> &held) {
  if constexpr (std::is_lvalue_reference_v<Argument> &&
                std::is_same_v<held_t<Argument>, std::remove_cvref_t<Argument>>)
    return static_cast<Argument>(held);
  else if constexpr (std::is_rvalue_reference_v<Argument>)
    return std::move(held);
  else
    return static_cast<std::remove_cvref_t<Argument>>(held);
}

// The parameters of a function, a pointer to one, or a lambda.
template <class F> struct Signature : Signature<decltype(&F::operator())> {};
template <class R, class... A> struct Signature<R (*)(A...)> {
  using result = R;
  using arguments = std::tuple<A...>;
};
template <class R, class... A>
struct Signature<R (*)(A...) noexcept> : Signature<R (*)(A...)> {};
template <class R, class C, class... A>
struct Signature<R (C::*)(A...) const> : Signature<R (*)(A...)> {};
template <class R, class C, class... A>
struct Signature<R (C::*)(A...) const noexcept> : Signature<R (*)(A...)> {};

template <std::size_t I, class Domains, class = void> struct DomainAt {
  using type = domain::Type;
};
template <std::size_t I, class Domains>
struct DomainAt<I, Domains, std::enable_if_t<(I < std::tuple_size_v<Domains>)>> {
  using type = std::tuple_element_t<I, Domains>;
};

// What the bench's operator new counted since the process began, so a function's
// allocations per call show (CPP10).
struct Allocated {
  std::uint64_t count = 0;
  std::uint64_t bytes = 0;
};
Allocated allocated();

// What a function's calls cost past each row's first, which the row records: time and
// allocations, as only the calls take them (CPP10, C03).
struct Spent {
  std::uint64_t calls = 0;
  std::uint64_t nanoseconds = 0;
  std::uint64_t allocations = 0;
  std::uint64_t bytes = 0;
};

// Where a function's rows go, a line each: printed as each call returns, after what the
// call printed itself, which bench.py reads as that row's own; skipped, as the first of
// two passes; kept, for one of two threads that run at once; or dropped, while a run only
// counts the cost.
class Rows {
public:
  enum class Mode { print, skip, keep, drop };
  Rows(Mode mode,
       std::filesystem::path root,
       std::filesystem::path repo,
       bool reverse = false,
       std::uint64_t repeat = 0);
  void row(std::string text);
  // Drops what was printed since the row before, as a scenario's setup prints it.
  void skip() const;
  // What body returns, what it printed dropped, as a row about an object that prints as
  // it is made and goes, whose rows show what it said another way.
  template <class F> auto quiet(F &&body) const {
    auto result = body();
    skip();
    return result;
  }
  // The handles of the row being written.
  Names &names();
  // The sandbox a case may make files in, which bench.py empties for each process.
  const std::filesystem::path &root() const;
  // The tree's top, which a world reads the projects and recipes from, never writes.
  const std::filesystem::path &repo() const;
  // Whether the rows run last to first, as the second pass of a context does.
  bool reverse() const;
  std::size_t count() const;
  std::vector<std::string> &kept();
  // How many times more a sweep calls each row's function, to count its cost; 0 but in a
  // cost run.
  std::uint64_t repeat() const;
  Spent &spent();

private:
  Mode _mode;
  std::filesystem::path _root;
  std::filesystem::path _repo;
  bool _reverse;
  Names _names;
  std::vector<std::string> _kept;
  std::size_t _count = 0;
  std::uint64_t _repeat;
  Spent _spent;
};

// What the call that is running threw, as a row shows it: the standard type it is, and
// its message.
std::string thrown();

// Rows per function: each edge of each parameter with the others typical, then pairs of
// edges across two parameters, then seeded random rows, so a function of three parameters
// with twenty edges each runs hundreds of rows, not eight thousand.
inline constexpr std::size_t row_cap = 300;
inline constexpr std::size_t random_rows = 48;

// A function's rows: first those its inputs' edges make, picked, then those drawn.
template <class Row> struct Made {
  std::vector<Row> rows;
  std::size_t picked = 0;
};

template <class... G, std::size_t... I>
Made<std::tuple<typename G::type...>> rows_of(Random &random, std::index_sequence<I...>) {
  using Row = std::tuple<typename G::type...>;
  const std::tuple<std::vector<typename G::type>...> edges{G::edges()...};
  const std::array<std::size_t, sizeof...(G)> counts{std::get<I>(edges).size()...};
  const auto set = [&](Row &row, std::size_t parameter, std::size_t edge) {
    ((I == parameter ? (void)(std::get<I>(row) = std::get<I>(edges)[edge]) : void()),
     ...);
  };
  const Row typical{std::get<I>(edges).front()...};
  std::vector<Row> rows{typical};
  for (std::size_t parameter = 0; parameter < counts.size(); ++parameter)
    for (std::size_t edge = 1; edge < counts[parameter]; ++edge)
      set(rows.emplace_back(typical), parameter, edge);
  std::size_t pairs = 0;
  for (std::size_t first = 0; first < counts.size(); ++first)
    for (std::size_t second = first + 1; second < counts.size(); ++second)
      pairs += (counts[first] - 1) * (counts[second] - 1);
  const std::size_t room = row_cap - std::min(row_cap, rows.size() + random_rows);
  const std::size_t stride = room == 0 ? 0 : (pairs + room - 1) / room;
  std::size_t pair = 0;
  for (std::size_t first = 0; first < counts.size() && stride; ++first)
    for (std::size_t second = first + 1; second < counts.size(); ++second)
      for (std::size_t one = 1; one < counts[first]; ++one)
        for (std::size_t other = 1; other < counts[second]; ++other)
          if (pair++ % stride == 0) {
            Row &row = rows.emplace_back(typical);
            set(row, first, one);
            set(row, second, other);
          }
  const std::size_t picked = rows.size();
  for (std::size_t made = 0; made < random_rows && rows.size() < row_cap; ++made)
    rows.push_back(Row{G::random(random)...});
  return {std::move(rows), picked};
}

// The rows to run, each input once, as the first row that has it, in either order.
template <class Row, std::size_t... I>
std::vector<std::size_t> distinct(const Made<Row> &made, std::index_sequence<I...>) {
  std::vector<std::string> seen;
  std::vector<std::size_t> kept;
  for (std::size_t index = 0; index < made.rows.size(); ++index) {
    Names names;
    std::string inputs;
    ((inputs.append(I ? ", " : "").append(show(std::get<I>(made.rows[index]), names))),
     ...);
    if (!std::ranges::binary_search(seen, inputs)) {
      seen.insert(std::ranges::upper_bound(seen, inputs), inputs);
      kept.push_back(index);
    }
  }
  return kept;
}

template <class Result, class... Arguments, class... Domains, class F, std::size_t... I>
void sweep_rows(Rows &rows,
                std::uint64_t seed,
                const F &function,
                std::tuple<Domains...> *,
                std::index_sequence<I...> indices) {
  using Generators = std::tuple<
      typename Source<typename DomainAt<I, std::tuple<Domains...>>::type,
                      held_t<std::tuple_element_t<I, std::tuple<Arguments...>>>>::gen...>;
  static_assert(
      (std::is_same_v<typename std::tuple_element_t<I, Generators>::type,
                      held_t<std::tuple_element_t<I, std::tuple<Arguments...>>>> &&
       ...),
      "a domain makes the type the bench holds for its parameter");
  Random random(seed);
  auto made = rows_of<std::tuple_element_t<I, Generators>...>(random, indices);
  std::vector<std::size_t> kept = distinct(made, indices);
  if (rows.reverse())
    std::ranges::reverse(kept);
  for (const std::size_t index : kept) {
    auto &row = made.rows[index];
    Names &names = rows.names();
    // A drawn row starts with ~, so a record lists the picked ones first.
    std::string text = index < made.picked ? "(" : "~(";
    ((text.append(I ? ", " : "").append(show(std::get<I>(row), names))), ...);
    text += ") -> ";
    try {
      if constexpr (std::is_void_v<Result>) {
        function(pass<Arguments>(std::get<I>(row))...);
        text += "done";
      } else {
        decltype(auto) result = function(pass<Arguments>(std::get<I>(row))...);
        text += show(result, names);
      }
    } catch (...) {
      text += thrown();
    }
    if (rows.repeat() > 0) {
      const Allocated before = allocated();
      const auto started = std::chrono::steady_clock::now();
      for (std::uint64_t again = 0; again < rows.repeat(); ++again)
        try {
          function(pass<Arguments>(std::get<I>(row))...);
        } catch (...) { // the row's first call said what it throws
        }
      const auto took = std::chrono::steady_clock::now() - started;
      const Allocated after = allocated();
      Spent &spent = rows.spent();
      spent.calls += rows.repeat();
      spent.nanoseconds += static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(took).count());
      spent.allocations += after.count - before.count;
      spent.bytes += after.bytes - before.bytes;
    }
    ((changed<Arguments> ? (void)text.append(std::format(
                               "; #{} = {}", I + 1, show(std::get<I>(row), names)))
                         : void()),
     ...);
    rows.row(std::move(text));
  }
}

template <class... Arguments, class... Domains, class F, std::size_t... I>
void holds_rows(Rows &rows,
                std::uint64_t seed,
                std::string_view claim,
                const F &predicate,
                std::tuple<Domains...> *,
                std::index_sequence<I...> indices) {
  using Generators = std::tuple<
      typename Source<typename DomainAt<I, std::tuple<Domains...>>::type,
                      held_t<std::tuple_element_t<I, std::tuple<Arguments...>>>>::gen...>;
  Random random(seed);
  auto made = rows_of<std::tuple_element_t<I, Generators>...>(random, indices);
  const std::vector<std::size_t> kept = distinct(made, indices);
  constexpr std::size_t shown_failures = 10;
  std::size_t failed = 0;
  for (const std::size_t index : kept) {
    auto &row = made.rows[index];
    Names &names = rows.names();
    std::string inputs;
    ((inputs.append(I ? ", " : "").append(show(std::get<I>(row), names))), ...);
    std::string outcome;
    try {
      if (!predicate(pass<Arguments>(std::get<I>(row))...))
        outcome = "false";
    } catch (...) {
      outcome = thrown();
    }
    if (outcome.empty() || ++failed > shown_failures) {
      names.clear();
      continue;
    }
    rows.row(std::format("fails: {}, at ({}): {}", claim, inputs, outcome));
  }
  rows.row(failed == 0
               ? std::format("holds: {}, over {} rows", claim, kept.size())
               : std::format("fails: {}, in {} of {} rows", claim, failed, kept.size()));
}

template <class Made, class F, class... Domains, class First, class... Arguments>
void sweep_made(Rows &rows,
                std::uint64_t seed,
                Made &made,
                const F &function,
                std::tuple<Domains...> *domains,
                std::tuple<First, Arguments...> *) {
  const auto bound = [&](Arguments... arguments) -> typename Signature<F>::result {
    return function(made, std::forward<Arguments>(arguments)...);
  };
  sweep_rows<typename Signature<F>::result, Arguments...>(
      rows, seed, bound, domains, std::index_sequence_for<Arguments...>{});
}

// A sweep's rows within a check, each call given first what the check made once for all
// of them, as a device, which each row would be slow to make again; the rest of its
// parameters get inputs as a sweep's do.
template <class Made, class F, class... Domains>
void sweep_in(
    Rows &rows, const std::string &id, Made &made, const F &function, Domains...) {
  sweep_made(rows,
             hash_of(id),
             made,
             function,
             static_cast<std::tuple<Domains...> *>(nullptr),
             static_cast<typename Signature<F>::arguments *>(nullptr));
}

// What a case file registers its functions with. Each id names what a function does, as
// Text.h/trim, not its signature, so its record stays while a refactor reshapes the call.
class Bench {
public:
  using Body = std::function<void(Rows &)>;

  // Rows a case writes itself, as a scenario of calls on one object.
  void check(const std::string &id, Body body);

  // A row per input: what the function returned or threw, and each parameter it may
  // change, as it stood after. A domain per parameter narrows its inputs to a meaning,
  // domain::Type keeping its type's own. A function whose first parameter is const
  // Rows & gets the rows it writes to, for their sandbox, and inputs for the others.
  template <class F, class... Domains>
  void sweep(const std::string &id, F function, Domains...) {
    check(id, [seed = hash_of(id), function](Rows &rows) {
      sweep_with(rows,
                 seed,
                 function,
                 static_cast<std::tuple<Domains...> *>(nullptr),
                 static_cast<typename Signature<F>::arguments *>(nullptr));
    });
  }

  // A claim that holds over every row, which reads one row more: where it fails, the
  // first failures and the count. The claim is a person's words, shown as written. A
  // predicate may take const Rows & first, as a sweep's function may.
  template <class F, class... Domains>
  void holds(const std::string &id, std::string claim, F predicate, Domains...) {
    check(id, [seed = hash_of(id + claim), claim, predicate](Rows &rows) {
      holds_with(rows,
                 seed,
                 claim,
                 predicate,
                 static_cast<std::tuple<Domains...> *>(nullptr),
                 static_cast<typename Signature<F>::arguments *>(nullptr));
    });
  }

  // A context a function's rows should follow, left out for it, with the why the report
  // shows, as Terminal::columns follows the terminal.
  void leaves_out(const std::string &id, std::string context, std::string why);

  // bench.py's way in: list the ids; run one id's rows in a context.
  int run(std::span<char *const> arguments);

private:
  template <class F, class... Domains, class... Arguments>
  static void sweep_with(Rows &rows,
                         std::uint64_t seed,
                         const F &function,
                         std::tuple<Domains...> *domains,
                         std::tuple<Arguments...> *) {
    sweep_rows<typename Signature<F>::result, Arguments...>(
        rows, seed, function, domains, std::index_sequence_for<Arguments...>{});
  }
  template <class F, class... Domains, class... Arguments>
  static void sweep_with(Rows &rows,
                         std::uint64_t seed,
                         const F &function,
                         std::tuple<Domains...> *domains,
                         std::tuple<const Rows &, Arguments...> *) {
    const auto bound = [&](Arguments... arguments) -> typename Signature<F>::result {
      return function(rows, std::forward<Arguments>(arguments)...);
    };
    sweep_rows<typename Signature<F>::result, Arguments...>(
        rows, seed, bound, domains, std::index_sequence_for<Arguments...>{});
  }
  template <class F, class... Domains, class... Arguments>
  static void holds_with(Rows &rows,
                         std::uint64_t seed,
                         std::string_view claim,
                         const F &predicate,
                         std::tuple<Domains...> *domains,
                         std::tuple<Arguments...> *) {
    holds_rows<Arguments...>(
        rows, seed, claim, predicate, domains, std::index_sequence_for<Arguments...>{});
  }
  template <class F, class... Domains, class... Arguments>
  static void holds_with(Rows &rows,
                         std::uint64_t seed,
                         std::string_view claim,
                         const F &predicate,
                         std::tuple<Domains...> *domains,
                         std::tuple<const Rows &, Arguments...> *) {
    const auto bound = [&](Arguments... arguments) {
      return predicate(rows, std::forward<Arguments>(arguments)...);
    };
    holds_rows<Arguments...>(
        rows, seed, claim, bound, domains, std::index_sequence_for<Arguments...>{});
  }

  struct Entry {
    std::string id;
    std::vector<Body> bodies;
    std::vector<std::pair<std::string, std::string>> left_out;
  };
  Entry &entry(const std::string &id);
  std::vector<Entry> _entries;
};

// A private member called on its own, though private first stays (CPP07): an explicit
// instantiation may name it, as the standard checks no access there, and the friend it
// defines hands its pointer on, found through a tag. Once per member, a case writes, in
// namespace VP::bench itself, as an explicit instantiation must be,
//
//   struct LogKept {
//     using type = bool (VP::Log::*)(VP::Level, std::string_view) const;
//     friend constexpr type reach(LogKept);
//   };
//   template struct Reach<LogKept, &VP::Log::kept>;
//
// and calls (log.*reach(LogKept{}))(VP::Level::info, "").
template <class Tag, typename Tag::type Member> struct Reach {
  friend constexpr typename Tag::type reach(Tag) {
    return Member;
  }
};

// As Reach, for a member whose type names a private type, as Pipelines' _frame does,
// which the tag cannot spell: the tag only declares the friend, and its type is deduced
// here. MSVC deduces it only where a function reads it into a local: inside a lambda it
// calls the type void, and as a range-for's range a syntax error. So a case reads it
// into a local outside both, and a type it can spell takes Reach.
//
//   struct PipelinesFrame {
//     friend constexpr auto reach(PipelinesFrame);
//   };
//   template struct Reached<PipelinesFrame, &VP::Pipelines::_frame>;
template <class Tag, auto Member> struct Reached {
  friend constexpr auto reach(Tag) {
    return Member;
  }
};

// The case file's: each function of its engine file, registered.
void cases(Bench &bench);

} // namespace VP::bench
