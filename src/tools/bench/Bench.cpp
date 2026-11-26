#include "tools/bench/Bench.h"

#include <chrono>
#include <clocale>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <locale>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <variant>

namespace VP::bench {

namespace {

constexpr std::uint64_t fnv_offset = 14695981039346656037ull;
constexpr std::uint64_t fnv_prime = 1099511628211ull;
// The bytes a terminal shows as they are.
constexpr unsigned char first_shown = 0x20;
constexpr unsigned char last_shown = 0x7e;

// The marks bench.py reads a run by. Each row ends with row_mark, and what the call
// printed before it is that row's own; a row of a pass bench.py drops ends with
// skip_mark. Standard error gets the same mark at each row, so what a call wrote there
// pairs with its row too. A cost line starts with cost_mark.
constexpr char row_mark = '\x1e';
constexpr char skip_mark = '\x1f';
constexpr char cost_mark = '\x1d';

// The heap shifted: blocks of many sizes, every other one freed again, so the rows run in
// memory laid out otherwise than the baseline's.
constexpr std::size_t heap_blocks = 1000;
constexpr std::size_t heap_stride = 37;
constexpr std::size_t heap_span = 4096;

void escape_into(std::string &out, std::string_view text) {
  for (const char letter : text) {
    const auto code = static_cast<unsigned char>(letter);
    switch (letter) {
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      default:
        if (code < first_shown || code > last_shown)
          out += std::format("\\x{:02x}", code);
        else
          out += letter;
    }
  }
}

// A row as one line of a record: the bytes that would end it there, or end it early in a
// run as bench.py reads it, shown escaped. A case may write what a call said whole, as a
// file's text with its newline, which would otherwise split into two rows, the second
// taken for what the next call printed.
void one_line(std::string &text) {
  constexpr std::string_view breaking = "\n\r\x1d\x1e\x1f";
  if (text.find_first_of(breaking) == std::string::npos)
    return;
  std::string out;
  for (const char letter : text)
    if (breaking.find(letter) == std::string_view::npos)
      out += letter;
    else
      escape_into(out, std::string_view(&letter, 1));
  text = std::move(out);
}

// A row of a thread that ran in a folder of its own, as if it had run in the root.
std::string as_in_root(std::string row,
                       const std::filesystem::path &folder,
                       const std::filesystem::path &root) {
  const std::string from = folder.generic_string();
  const std::string to = root.generic_string();
  for (std::size_t at = from.empty() ? std::string::npos : row.find(from);
       at != std::string::npos;
       at = row.find(from, at + to.size()))
    row.replace(at, from.size(), to);
  return row;
}

void frame(char mark, std::string_view text) {
  std::cout.flush(); // what the call printed comes before its row
  std::string line(1, mark);
  line.append(text).append("\n");
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fflush(stdout);
  std::cerr.flush();
  const std::array<char, 2> end{row_mark, '\n'};
  std::fwrite(end.data(), 1, end.size(), stderr);
  std::fflush(stderr);
}

struct Options {
  std::string_view context = "baseline";
  std::optional<std::string_view> locale;
  std::optional<std::uint64_t> repeat;
  std::filesystem::path root;
  std::filesystem::path repo;
};

std::optional<Options> options_of(std::span<char *const> arguments) {
  Options options;
  for (std::size_t index = 0; index + 1 < arguments.size(); index += 2) {
    const std::string_view key = arguments[index];
    const std::string_view value = arguments[index + 1];
    if (key == "--context")
      options.context = value;
    else if (key == "--locale")
      options.locale = value;
    else if (key == "--root")
      options.root = std::filesystem::path(value);
    else if (key == "--repo")
      options.repo = std::filesystem::path(value);
    else if (key == "--repeat") {
      std::uint64_t count = 0;
      const auto [end, error] =
          std::from_chars(value.data(), value.data() + value.size(), count);
      if (error != std::errc{} || end != value.data() + value.size())
        return std::nullopt;
      options.repeat = count;
    } else
      return std::nullopt;
  }
  if (arguments.size() % 2 != 0)
    return std::nullopt;
  return options;
}

int usage() {
  std::fputs("usage: bench-<File> list\n"
             "       bench-<File> run ID [--context baseline|again|thread|threads|heap]\n"
             "                     [--locale NAME] [--repeat N] [--root FOLDER]\n"
             "                     [--repo FOLDER]\n",
             stderr);
  return 2;
}

using Folder = std::filesystem::path;

// Each sweep's rows once, as ever, and each row's call that many times more, which
// alone are counted: a first call's allocations are its own (CPP10).
int cost(const Bench::Body &pass,
         const Folder &root,
         const Folder &repo,
         std::uint64_t repeat) {
  Rows rows(Rows::Mode::drop, root, repo, false, repeat);
  pass(rows);
  const Spent &spent = rows.spent();
  std::printf("%ccost rows=%zu calls=%llu nanoseconds=%llu allocations=%llu bytes=%llu\n",
              cost_mark,
              rows.count(),
              static_cast<unsigned long long>(spent.calls),
              static_cast<unsigned long long>(spent.nanoseconds),
              static_cast<unsigned long long>(spent.allocations),
              static_cast<unsigned long long>(spent.bytes));
  return 0;
}

void baseline(const Bench::Body &pass, const Folder &root, const Folder &repo) {
  Rows rows(Rows::Mode::print, root, repo);
  pass(rows);
}

void again(const Bench::Body &pass, const Folder &root, const Folder &repo) {
  Rows first(Rows::Mode::skip, root, repo);
  pass(first);
  Rows second(Rows::Mode::print, root, repo, true);
  pass(second);
}

void thread(const Bench::Body &pass, const Folder &root, const Folder &repo) {
  Rows rows(Rows::Mode::print, root, repo);
  std::thread([&] { pass(rows); }).join();
}

// Each in a folder of its own, which bench.py makes, as the bench writes no file (RA04);
// their rows then read as if they had run in the root.
void threads(const Bench::Body &pass, const Folder &root, const Folder &repo) {
  const Folder first_root = root.empty() ? root : root / "one";
  const Folder second_root = root.empty() ? root : root / "two";
  Rows one(Rows::Mode::keep, first_root, repo);
  Rows two(Rows::Mode::keep, second_root, repo);
  {
    std::jthread first([&] { pass(one); });
    std::jthread second([&] { pass(two); });
  }
  frame(skip_mark, {}); // what the threads printed belongs to no one row
  for (std::size_t index = 0; index < std::max(one.kept().size(), two.kept().size());
       ++index) {
    const auto row = [&](Rows &rows, const Folder &folder) {
      return index < rows.kept().size() ? as_in_root(rows.kept()[index], folder, root)
                                        : std::string("no row");
    };
    const std::string mine = row(one, first_root);
    const std::string other = row(two, second_root);
    frame(row_mark, mine);
    if (other != mine)
      frame(row_mark, "!! beside it at once: " + other);
  }
}

void heap(const Bench::Body &pass, const Folder &root, const Folder &repo) {
  std::vector<std::unique_ptr<char[]>> shifted;
  for (std::size_t index = 0; index < heap_blocks; ++index)
    shifted.push_back(std::make_unique<char[]>(index * heap_stride % heap_span + 1));
  for (std::size_t index = 0; index < shifted.size(); index += 2)
    shifted[index].reset();
  Rows rows(Rows::Mode::print, root, repo);
  pass(rows);
}

// The contexts by the names bench.py gives them.
constexpr std::array contexts{std::pair{std::string_view{"baseline"}, &baseline},
                              std::pair{std::string_view{"again"}, &again},
                              std::pair{std::string_view{"thread"}, &thread},
                              std::pair{std::string_view{"threads"}, &threads},
                              std::pair{std::string_view{"heap"}, &heap}};

// Refused where this machine has no such locale.
bool set_locale(const std::string &name) {
  try {
    std::locale::global(std::locale(name));
  } catch (const std::runtime_error &) {
    std::fprintf(stderr, "no locale %s on this machine\n", name.c_str());
    return false;
  }
  std::setlocale(LC_ALL, name.c_str());
  return true;
}

} // namespace

std::size_t Names::number(const void *handle) {
  const auto found = std::ranges::find(_seen, handle);
  if (found != _seen.end())
    return static_cast<std::size_t>(found - _seen.begin()) + 1;
  _seen.push_back(handle);
  return _seen.size();
}

void Names::clear() {
  _seen.clear();
}

std::string escaped(std::string_view text) {
  std::string out = "\"";
  escape_into(out, text);
  return out + '"';
}

std::uint64_t hash_of(std::string_view text) {
  std::uint64_t hash = fnv_offset;
  for (const char letter : text)
    hash = (hash ^ static_cast<unsigned char>(letter)) * fnv_prime;
  return hash;
}

std::vector<std::string> Gen<std::string>::edges() {
  using namespace std::string_literals;
  return {"a",   "",     " ",   "ab",    "a b",      " a",
          "a ",  "\t",   "\r",  "\n",    "a\n",      "a\r\n",
          "\na", "a\nb", "\0"s, "a\0b"s, "\xc3\xa9", "\xff",
          "1",   "-1",   "a.b", "a=b",   "#",        "\"",
          "\\",  ",",    ".",   "..",    "/",        std::string(300, 'a')};
}

std::string Gen<std::string>::random(Random &random) {
  using namespace std::string_view_literals; // the NUL too
  constexpr std::string_view letters = "aAz09 .,=-_#/\\\"\t\r\n\0\xc3\xa9\xff"sv;
  constexpr std::uint64_t longest = 12;
  std::string text(random.below(longest), ' ');
  for (char &letter : text)
    letter = letters[random.below(letters.size())];
  return text;
}

std::vector<std::filesystem::path> Gen<std::filesystem::path>::edges() {
  std::vector<std::filesystem::path> all;
  for (const std::u8string_view text : {u8"a",
                                        u8"",
                                        u8".",
                                        u8"..",
                                        u8"a/b",
                                        u8"a/../b",
                                        u8"./a",
                                        u8"a/",
                                        u8"a b",
                                        u8".hidden",
                                        u8"a/.b/c",
                                        u8"é",
                                        u8"a//b",
                                        u8"a/./b",
                                        u8"../a"})
    all.emplace_back(text);
  return all;
}

std::filesystem::path Gen<std::filesystem::path>::random(Random &random) {
  constexpr std::array<std::u8string_view, 6> parts{
      u8"a", u8"b", u8"..", u8".", u8".c", u8"d e"};
  constexpr std::uint64_t deepest = 4;
  std::filesystem::path path;
  for (std::uint64_t part = random.below(deepest); part > 0; --part)
    path /= std::filesystem::path(parts[random.below(parts.size())]);
  return path;
}

Rows::Rows(Mode mode,
           std::filesystem::path root,
           std::filesystem::path repo,
           bool reverse,
           std::uint64_t repeat)
    : _mode(mode), _root(std::move(root)), _repo(std::move(repo)), _reverse(reverse),
      _repeat(repeat) {}

void Rows::row(std::string text) {
  ++_count;
  _names.clear();
  one_line(text);
  switch (_mode) {
    case Mode::print:
      frame(row_mark, text);
      break;
    case Mode::skip:
      frame(skip_mark, {});
      break;
    case Mode::keep:
      _kept.push_back(std::move(text));
      break;
    case Mode::drop:
      break;
  }
}

void Rows::skip() const {
  if (_mode == Mode::print || _mode == Mode::skip)
    frame(skip_mark, {});
}

Names &Rows::names() {
  return _names;
}

const std::filesystem::path &Rows::root() const {
  return _root;
}

const std::filesystem::path &Rows::repo() const {
  return _repo;
}

bool Rows::reverse() const {
  return _reverse;
}

std::size_t Rows::count() const {
  return _count;
}

std::vector<std::string> &Rows::kept() {
  return _kept;
}

std::uint64_t Rows::repeat() const {
  return _repeat;
}

Spent &Rows::spent() {
  return _spent;
}

// Most derived first, as a handler takes what derives from its type.
std::string thrown() {
  const auto said = [](std::string_view kind, const std::exception &error) {
    return std::format("throws {}: {}", kind, escaped(error.what()));
  };
  try {
    throw;
  } catch (const std::bad_alloc &) {
    return "throws bad_alloc";
  } catch (const std::invalid_argument &error) {
    return said("invalid_argument", error);
  } catch (const std::out_of_range &error) {
    return said("out_of_range", error);
  } catch (const std::length_error &error) {
    return said("length_error", error);
  } catch (const std::domain_error &error) {
    return said("domain_error", error);
  } catch (const std::logic_error &error) {
    return said("logic_error", error);
  } catch (const std::filesystem::filesystem_error &error) {
    return said("filesystem_error", error);
  } catch (const std::system_error &error) {
    return said("system_error", error);
  } catch (const std::range_error &error) {
    return said("range_error", error);
  } catch (const std::overflow_error &error) {
    return said("overflow_error", error);
  } catch (const std::underflow_error &error) {
    return said("underflow_error", error);
  } catch (const std::runtime_error &error) {
    return said("runtime_error", error);
  } catch (const std::bad_optional_access &) {
    return "throws bad_optional_access";
  } catch (const std::bad_variant_access &) {
    return "throws bad_variant_access";
  } catch (const std::exception &error) {
    return said("exception", error);
  } catch (...) {
    return "throws something not a std::exception";
  }
}

Bench::Entry &Bench::entry(const std::string &id) {
  const auto found = std::ranges::find(_entries, id, &Entry::id);
  return found != _entries.end() ? *found : _entries.emplace_back(Entry{.id = id});
}

void Bench::check(const std::string &id, Body body) {
  entry(id).bodies.push_back(std::move(body));
}

void Bench::leaves_out(const std::string &id, std::string context, std::string why) {
  entry(id).left_out.emplace_back(std::move(context), std::move(why));
}

// One id's rows in one context: once; twice, the second pass last row first; on another
// thread; on two threads at once, each in a folder of its own, whose rows must be alike;
// or in a shifted heap. With --repeat, the rows run that many times more, printing only
// what they cost.
int Bench::run(std::span<char *const> arguments) {
  // An id no check registers, as a context left out for a misspelt one makes, would be
  // recorded with no rows and pass every compare: refused, naming it (A02).
  for (const Entry &entry : _entries)
    if (entry.bodies.empty()) {
      std::fprintf(stderr,
                   "%s: a context is left out for it, but no check registers it\n",
                   entry.id.c_str());
      return 2;
    }
  if (arguments.size() == 2 && std::string_view(arguments[1]) == "list") {
    for (const Entry &entry : _entries) {
      std::printf("%s\n", entry.id.c_str());
      for (const auto &[context, why] : entry.left_out)
        std::printf("\t%s\t%s\n", context.c_str(), why.c_str());
    }
    return 0;
  }
  if (arguments.size() < 3 || std::string_view(arguments[1]) != "run")
    return usage();
  const std::string_view id = arguments[2];
  const std::optional<Options> options = options_of(arguments.subspan(3));
  if (!options)
    return usage();
  const auto found = std::ranges::find(_entries, id, &Entry::id);
  if (found == _entries.end()) {
    std::fprintf(stderr,
                 "no function %.*s in this bench\n",
                 static_cast<int>(id.size()),
                 id.data());
    return 2;
  }
  if (options->locale && !set_locale(std::string(*options->locale)))
    return 2;
  const Body pass = [&](Rows &rows) {
    for (const Body &body : found->bodies)
      body(rows);
  };
  if (options->repeat)
    return cost(pass, options->root, options->repo, *options->repeat);
  const auto context = std::ranges::find_if(
      contexts, [&](const auto &each) { return each.first == options->context; });
  if (context == contexts.end())
    return usage();
  context->second(pass, options->root, options->repo);
  return 0;
}

} // namespace VP::bench
