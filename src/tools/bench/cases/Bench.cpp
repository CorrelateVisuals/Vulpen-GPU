#include "tools/bench/Bench.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <ios>
#include <memory>
#include <new>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

// tools/bench/Bench.h and Bench.cpp: how a case's functions become rows, which
// every record of the review goes through, so a fault here would move, or worse hide, a
// change in every record. These rows hold the bench to its comments: a row per input,
// each input once, a claim's first failures and their count, what a call threw by its
// most derived standard type, and a run's words read or refused.
//
// And the canaries: rows planted to move in one context or build each, as nothing else
// bench.py varies moves them. bench.py run fails where a canary/<context> did not move
// in its context, so a context that stopped reaching the process fails the run, instead
// of calling every function the same there.

namespace VP::bench {

namespace {

// The words a bench is started with, as bench.py and a person start it, each refused or
// read. None runs rows, whose marks would end this check's own row; threads is left out,
// as it marks a row's start even when there is none.
std::vector<std::vector<std::string>> words_tried() {
  return {{"bench-x", "list"},
          {"bench-x", "list", "more"},
          {"bench-x"},
          {"bench-x", "lists"},
          {"bench-x", "run"},
          {"bench-x", "run", "none"},
          {"bench-x", "run", "an id"},
          {"bench-x", "run", "an id", "--context"},
          {"bench-x", "run", "an id", "--context", "again"},
          {"bench-x", "run", "an id", "--context", "thread"},
          {"bench-x", "run", "an id", "--context", "heap"},
          {"bench-x", "run", "an id", "--context", "nowhere"},
          {"bench-x", "run", "an id", "--repeat", "3"},
          {"bench-x", "run", "an id", "--repeat", "x"},
          {"bench-x", "run", "an id", "--repeat", "-1"},
          {"bench-x", "run", "an id", "--repeat", "3 "},
          {"bench-x", "run", "an id", "--repeat", "18446744073709551616"},
          {"bench-x", "run", "an id", "--locale", "xx_XX.none"},
          {"bench-x", "run", "an id", "--root"},
          {"bench-x", "run", "an id", "--root", "a", "--repo", "b"},
          {"bench-x", "run", "an id", "--colour", "red"}};
}

// A bench of its own, as a case file registers one: an id twice and a context left out;
// with misspelt, also a context left out for an id no check registers.
int run_inner(std::vector<std::string> words, bool misspelt = false) {
  Bench inner;
  inner.check("an id", [](Rows &) {});
  inner.check("another", [](Rows &) {});
  inner.check("an id", [](Rows &) {});
  inner.leaves_out("an id", "thread", "a why");
  if (misspelt)
    inner.leaves_out("an idd", "heap", "a why");
  std::vector<char *> arguments;
  for (std::string &word : words)
    arguments.push_back(word.data());
  return inner.run(arguments);
}

// What thrown() makes of what raise throws; a message the library writes, which differs
// between libraries, cut to its kind.
template <class Raise> std::string thrown_by(Raise raise, bool library_message) {
  try {
    raise();
  } catch (...) {
    const std::string said = thrown();
    return library_message ? said.substr(0, said.find(':')) : said;
  }
  return "threw nothing";
}

template <class... G> Made<std::tuple<typename G::type...>> made(std::uint64_t seed) {
  Random random(seed);
  return rows_of<G...>(random, std::index_sequence_for<G...>{});
}

// A parameter with N edges, 0 to N - 1, so rows_of's check meets the cap's edges: 12
// and 21 edges, whose 11 × 20 pairs fill exactly the room the cap leaves, and 260, more
// single edges than that room, after which the drawn rows stop at the cap.
template <int N> struct Edges {
  using type = int;
  static std::vector<int> edges() {
    std::vector<int> all(N);
    std::iota(all.begin(), all.end(), 0);
    return all;
  }
  static int random(Random &random) {
    return static_cast<int>(random.below(N));
  }
};
static_assert(12 * 21 == row_cap - random_rows && 260 > row_cap - random_rows,
              "the edges of rows_of's check meet the cap as its comment says");

// Each row's inputs as the bench shows them, which is how it tells inputs apart.
template <class Row, std::size_t... I>
std::vector<std::string> shown_inputs(const std::vector<Row> &rows,
                                      std::index_sequence<I...>) {
  std::vector<std::string> shown;
  for (const Row &row : rows) {
    Names names;
    std::string inputs;
    ((inputs.append(I ? ", " : "").append(show(std::get<I>(row), names))), ...);
    shown.push_back(std::move(inputs));
  }
  return shown;
}

// The rows each edge alone makes, the others typical: the first row, then one per edge
// past each parameter's first.
template <class... G> std::size_t singles() {
  return 1 + ((G::edges().size() - 1) + ...);
}

template <class G, std::size_t I, class Row>
bool every_edge_in(const std::vector<Row> &rows) {
  Names names;
  return std::ranges::all_of(G::edges(), [&](const typename G::type &edge) {
    const std::string wanted = show(edge, names);
    return std::ranges::any_of(rows, [&](const Row &row) {
      Names own;
      return show(std::get<I>(row), own) == wanted;
    });
  });
}

// The claims each set of parameters is held to; one per set, as a function the bench
// sweeps has one, two, three or four parameters.
template <class... G, std::size_t... I>
bool rows_as_said(std::uint64_t seed, std::index_sequence<I...> indices) {
  const auto rows = made<G...>(seed);
  const auto first = shown_inputs(std::vector{rows.rows.front()}, indices).front();
  const auto typical =
      shown_inputs(std::vector{std::tuple<typename G::type...>{G::edges().front()...}},
                   indices)
          .front();
  return first == typical && (every_edge_in<G, I>(rows.rows) && ...) &&
         rows.picked <= rows.rows.size() &&
         rows.rows.size() - rows.picked <= random_rows &&
         rows.rows.size() <= std::max(row_cap, singles<G...>()) &&
         shown_inputs(rows.rows, indices) == shown_inputs(made<G...>(seed).rows, indices);
}

template <class... G> bool rows_as_said(std::uint64_t seed) {
  return rows_as_said<G...>(seed, std::index_sequence_for<G...>{});
}

template <class... G> bool distinct_as_said(std::uint64_t seed) {
  const auto indices = std::index_sequence_for<G...>{};
  const auto rows = made<G...>(seed);
  const std::vector<std::size_t> kept = distinct(rows, indices);
  const std::vector<std::string> shown = shown_inputs(rows.rows, indices);
  std::vector<std::string> kept_shown;
  for (const std::size_t index : kept)
    kept_shown.push_back(shown[index]);
  auto sorted = kept_shown;
  std::ranges::sort(sorted);
  return std::ranges::adjacent_find(sorted) == sorted.end() &&
         std::ranges::is_sorted(kept) &&
         std::ranges::all_of(shown, [&](const std::string &inputs) {
           return std::ranges::binary_search(sorted, inputs);
         });
}

template <class... G> std::string counted(std::uint64_t seed) {
  const auto rows = made<G...>(seed);
  return std::format("{} rows, {} picked, {} distinct",
                     rows.rows.size(),
                     rows.picked,
                     distinct(rows, std::index_sequence_for<G...>{}).size());
}

// What one sweep or claim writes, kept rather than printed, so a row of this check can
// show them.
template <class Body> std::string kept_by(const Rows &rows, bool reverse, Body body) {
  Rows inner(Rows::Mode::keep, rows.root(), rows.repo(), reverse);
  body(inner);
  Names names;
  return show(inner.kept(), names);
}

constexpr std::tuple<> *no_domains = nullptr;

void canaries(Bench &bench) {
  // What a second pass finds: the count the lambda keeps between the passes of again,
  // as a function that remembers its calls would.
  bench.check("canary/again", [calls = 0](Rows &rows) mutable {
    rows.row(std::format("call {}", ++calls));
  });
  bench.leaves_out("canary/again", "threads", "both threads would count on one number");
  // The order again's second pass takes, last row first, as a sweep's rows then run, so
  // what a row leaves behind for the next shows; the count above moves even without it.
  bench.check("canary/again, last to first", [](Rows &rows) {
    rows.row(rows.reverse() ? "rows last to first" : "rows first to last");
  });
  // The thread a row runs on: cases() runs on the process's main thread, as every
  // context but thread and threads runs the rows.
  bench.check("canary/thread", [registering = std::this_thread::get_id()](Rows &rows) {
    rows.row(std::this_thread::get_id() == registering
                 ? "on the thread that registered it"
                 : "on another thread");
  });
  // The folder a row runs in: of two threads at once, each has one of its own, whose rows
  // the threads context must tell apart.
  bench.check("canary/threads", [](Rows &rows) {
    rows.row("in the sandbox's " + escaped(rows.root().filename().generic_string()));
  });
  // What bench.py sets for the heap context alone, with which glibc fills freed memory.
  bench.check("canary/heap", [](Rows &rows) {
    const char *const perturb = std::getenv("MALLOC_PERTURB_");
    rows.row(std::format("MALLOC_PERTURB_ {}", perturb ? escaped(perturb) : "unset"));
  });
  // A file of the working folder, which only the cwd context fills.
  bench.check("canary/cwd", [](Rows &rows) {
    std::error_code error;
    rows.row(std::filesystem::exists("view.vlp", error)
                 ? "a view.vlp in the working folder"
                 : "no view.vlp in the working folder");
  });
  // A number as the C library prints it, which the locale context gives a decimal comma.
  bench.check("canary/locale", [](Rows &rows) {
    std::array<char, 16> text{};
    std::snprintf(text.data(), text.size(), "%.1f", 1.5);
    rows.row("1.5 printed as " + escaped(text.data()));
  });
  // A variable only the environment context sets.
  bench.check("canary/environment", [](Rows &rows) {
    const char *const term = std::getenv("TERM");
    rows.row(std::format("TERM {}", term ? escaped(term) : "unset"));
  });
  // An assert with a side effect, which a release build compiles out (NDEBUG).
  bench.check("canary/release", [](Rows &rows) {
    int asserted = 0;
    assert(++asserted == 1);
    rows.row(std::format("an assert's side effect: {}", asserted));
  });
  // A byte past ASCII as a number, as code reading a file's bytes takes one, which the
  // odd build reads as ARM does, its char unsigned; an ARM machine's own debug build
  // would give the odd row. Through a function, as the compiler sees no char in the
  // comparison then.
  bench.check("canary/odd", [](Rows &rows) {
    const auto as_number = [](char letter) { return static_cast<int>(letter); };
    rows.row(as_number('\xe9') < 0 ? "a byte past ASCII reads as negative"
                                   : "a byte past ASCII reads as positive");
  });
  // A block never freed, which the asan build's leak check reports as the process ends.
  bench.check("canary/asan", [](Rows &rows) {
    static_cast<void>(std::make_unique<int>(1).release());
    rows.row("a block left unfreed");
  });
}

void thrown_rows(Bench &bench) {
  // Each standard exception as a row shows it, by its own type before any it derives
  // from, as a handler takes what derives from its type.
  bench.check("Bench.cpp/thrown", [](Rows &rows) {
    const auto shows = [&](std::string_view what, auto raise, bool library = false) {
      rows.row(std::format("{} -> {}", what, thrown_by(raise, library)));
    };
    const std::error_code missing =
        std::make_error_code(std::errc::no_such_file_or_directory);
    shows("bad_alloc", [] { throw std::bad_alloc(); });
    shows("bad_array_new_length", [] { throw std::bad_array_new_length(); });
    shows("invalid_argument", [] { throw std::invalid_argument("a"); });
    shows("out_of_range", [] { throw std::out_of_range("a"); });
    shows("length_error", [] { throw std::length_error("a"); });
    shows("domain_error", [] { throw std::domain_error("a"); });
    shows("logic_error", [] { throw std::logic_error("a"); });
    shows(
        "filesystem_error",
        [&] { throw std::filesystem::filesystem_error("a", missing); },
        true);
    shows("system_error", [&] { throw std::system_error(missing, "a"); }, true);
    shows("ios_base::failure", [] { throw std::ios_base::failure("a"); }, true);
    shows("range_error", [] { throw std::range_error("a"); });
    shows("overflow_error", [] { throw std::overflow_error("a"); });
    shows("underflow_error", [] { throw std::underflow_error("a"); });
    shows("format_error", [] { throw std::format_error("a"); });
    shows("runtime_error", [] { throw std::runtime_error("a"); });
    shows("bad_optional_access", [] { static_cast<void>(std::optional<int>().value()); });
    shows("bad_variant_access",
          [] { static_cast<void>(std::get<1>(std::variant<int, char>())); });
    shows("bad_function_call", [] { std::function<void()>()(); }, true);
    shows("exception", [] { throw std::exception(); }, true);
    shows("an int", [] { throw 1; });
    shows("nothing", [] {});
  });
}

void rows_rows(Bench &bench) {
  // Rows kept and dropped, as two threads at once and a cost run write them; the handles
  // of a row numbered afresh at the next.
  bench.check("Bench.cpp/Rows", [](Rows &rows) {
    Rows kept(Rows::Mode::keep, rows.root(), rows.repo());
    kept.row("one");
    kept.skip();
    kept.row("two");
    Names names;
    rows.row(std::format("keep, two rows and a skip -> count {}, kept {}",
                         kept.count(),
                         show(kept.kept(), names)));
    Rows dropped(Rows::Mode::drop, rows.root(), rows.repo(), true, 3);
    dropped.row("one");
    rows.row(
        std::format("drop, reverse, repeat 3, a row -> count {}, kept {}, reverse {}, "
                    "repeat {}",
                    dropped.count(),
                    show(dropped.kept(), names),
                    dropped.reverse(),
                    dropped.repeat()));
    Rows lines(Rows::Mode::keep, rows.root(), rows.repo());
    lines.row("a\nb\r\x1e"
              "c");
    rows.row(std::format("a row holding a newline, a return and a mark -> kept {}",
                         show(lines.kept(), names)));
    const int one = 1;
    const int other = 2;
    const std::size_t before = kept.names().number(&one);
    kept.row("three");
    rows.row(
        std::format("a handle numbered, then a row -> the next handle is #{} (was #{})",
                    kept.names().number(&other),
                    before));
  });
}

void skipped_rows(Bench &bench) {
  // What a scenario's setup prints, taken by a skip or by quiet, so the row after it
  // carries none of it, as a case's rows show what a call printed and nothing else.
  bench.check("Bench.cpp/Rows::skip", [](Rows &rows) {
    std::printf("a setup's line\n");
    rows.skip();
    rows.row("a row after a skip");
    const int made = rows.quiet([] {
      std::printf("made\n");
      return 1;
    });
    rows.row(std::format("a row after quiet, which made {}", made));
  });
}

void run_rows(Bench &bench) {
  // A bench's words: listed, refused with its usage or a reason, or run, here with no
  // rows.
  bench.check("Bench.cpp/Bench::run", [](Rows &rows) {
    for (const std::vector<std::string> &words : words_tried()) {
      Names names;
      rows.row(std::format("{} -> {}", show(words, names), run_inner(words)));
    }
    rows.row(
        std::format("a context left out for an id no check registers, then list -> {}",
                    run_inner({"bench-x", "list"}, true)));
  });
}

void made_rows(Bench &bench) {
  using Int = Gen<int>;
  using Text = Gen<std::string>;
  using Number = Gen<float>;
  using Flag = Gen<bool>;
  bench.check("Bench.h/rows_of", [](Rows &rows) {
    rows.row("bool -> " + counted<Flag>(1));
    rows.row("int -> " + counted<Int>(1));
    rows.row("string -> " + counted<Text>(1));
    rows.row("int, string -> " + counted<Int, Text>(1));
    rows.row("string, string, int -> " + counted<Text, Text, Int>(1));
    rows.row("float, float, float, float -> " +
             counted<Number, Number, Number, Number>(1));
    rows.row("12 edges, 21 edges, the pairs filling the room -> " +
             counted<Edges<12>, Edges<21>>(1));
    rows.row("260 edges, more than the room -> " + counted<Edges<260>>(1));
  });
  bench.holds("Bench.h/rows_of",
              "the first row typical, every edge in a row, the drawn last and within the "
              "cap, the same seed the same rows",
              [](std::uint64_t seed) {
                return rows_as_said<Flag>(seed) && rows_as_said<Int>(seed) &&
                       rows_as_said<Int, Text>(seed) &&
                       rows_as_said<Text, Text, Int>(seed) &&
                       rows_as_said<Number, Number, Number, Number>(seed);
              });
  bench.holds("Bench.h/distinct",
              "each input once, the first row that has it, in order",
              [](std::uint64_t seed) {
                return distinct_as_said<Flag>(seed) && distinct_as_said<Int>(seed) &&
                       distinct_as_said<Int, Text>(seed) &&
                       distinct_as_said<Text, Text, Int>(seed);
              });
}

void sweep_rows_rows(Bench &bench) {
  // A sweep's rows, and a claim's, as the bench writes them for every case.
  bench.check("Bench.h/sweep_rows", [](Rows &rows) {
    const auto flip = [](bool &value) { value = !value; };
    const auto refuse = [](bool value) {
      if (value)
        throw std::invalid_argument("yes");
      return 1;
    };
    const auto twice = [](int value) { return value * 2LL; };
    rows.row("a void function that changes its parameter -> " +
             kept_by(rows, false, [&](Rows &inner) {
               sweep_rows<void, bool &>(
                   inner, 1, flip, no_domains, std::index_sequence<0>{});
             }));
    rows.row("one that throws for one input -> " + kept_by(rows, false, [&](Rows &inner) {
               sweep_rows<int, bool>(
                   inner, 1, refuse, no_domains, std::index_sequence<0>{});
             }));
    rows.row("in reverse -> " + kept_by(rows, true, [&](Rows &inner) {
               sweep_rows<int, bool>(
                   inner, 1, refuse, no_domains, std::index_sequence<0>{});
             }));
    rows.row("an int's, the drawn after the picked -> " +
             kept_by(rows, false, [&](Rows &inner) {
               sweep_rows<long long, int>(
                   inner, 1, twice, no_domains, std::index_sequence<0>{});
             }));
  });
  bench.check("Bench.h/holds_rows", [](Rows &rows) {
    const auto even = [](int value) { return value % 2 == 0; };
    const auto any = [](int) { return true; };
    const auto throws = [](int value) {
      if (value == 7)
        throw std::out_of_range("seven");
      return true;
    };
    rows.row("a claim false for odd numbers -> " + kept_by(rows, false, [&](Rows &inner) {
               holds_rows<int>(
                   inner, 1, "even", even, no_domains, std::index_sequence<0>{});
             }));
    rows.row("one true of every input -> " + kept_by(rows, false, [&](Rows &inner) {
               holds_rows<int>(
                   inner, 1, "any", any, no_domains, std::index_sequence<0>{});
             }));
    rows.row("one that throws for 7 -> " + kept_by(rows, false, [&](Rows &inner) {
               holds_rows<int>(
                   inner, 1, "not seven", throws, no_domains, std::index_sequence<0>{});
             }));
  });
}

} // namespace

void cases(Bench &bench) {
  canaries(bench);
  thrown_rows(bench);
  rows_rows(bench);
  skipped_rows(bench);
  run_rows(bench);
  made_rows(bench);
  sweep_rows_rows(bench);
}

} // namespace VP::bench
