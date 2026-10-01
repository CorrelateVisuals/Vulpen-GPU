#include "runtime/Commands.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <stdexcept>
#include <string>

namespace VP {

namespace {

constexpr std::string_view blanks = " \t\r";

std::vector<std::string_view> words_of(std::string_view line) {
  std::vector<std::string_view> words;
  for (std::size_t at = line.find_first_not_of(blanks); at != std::string_view::npos;) {
    const std::size_t end = std::min(line.find_first_of(blanks, at), line.size());
    words.push_back(line.substr(at, end - at));
    at = line.find_first_not_of(blanks, end);
  }
  return words;
}

bool lowercase(std::string_view word) {
  return std::ranges::all_of(word,
                             [](char letter) { return letter >= 'a' && letter <= 'z'; });
}

class Run final : public Call {
public:
  explicit Run(Command command) : _command(command) {}

private:
  bool is(Command command) const override {
    return command.index == _command.index;
  }

  const Command _command;
};

} // namespace

struct Commands::Spec {
  std::vector<std::string> name; // the words a line starts with
  std::string usage;
  std::string help;
  Command command;
  CommandHandler *handler = nullptr;
};

Commands::Commands(const Log &log)
    : _log(log), _quit(add("quit", "ends the run before its next frame", *this)) {}

Commands::~Commands() = default;

Command
Commands::add(std::string_view usage, std::string_view help, CommandHandler &handler) {
  const std::vector<std::string_view> words = words_of(usage);
  if (words.empty() || help.empty())
    throw std::runtime_error(
        std::format("command `{}` needs a usage and a help to register (RV04)", usage));
  if (!std::ranges::all_of(words, lowercase))
    throw std::runtime_error(
        std::format("command `{}`: a command's name is lowercase words", usage));
  if (const Spec *const twin = match(words); twin && twin->name.size() == words.size())
    throw std::runtime_error(std::format("command `{}` registers twice", usage));
  // From 1, so a Command no add returned matches none.
  const Command command{static_cast<std::uint32_t>(_specs.size() + 1)};
  _specs.push_back({.name = {words.begin(), words.end()},
                    .usage = std::string(usage),
                    .help = std::string(help),
                    .command = command,
                    .handler = &handler});
  return command;
}

void Commands::run(std::string_view line) {
  const std::vector<std::string_view> words = words_of(line);
  if (words.empty())
    return;
  const std::string_view typed(words.front().data(),
                               words.back().data() + words.back().size());
  const Spec *const spec = match(words);
  if (!spec) {
    std::string usages;
    for (const Spec &each : _specs)
      usages += (usages.empty() ? "" : ", ") + each.usage;
    throw std::runtime_error(
        std::format("unknown command {}; the commands are {}", words.front(), usages));
  }
  if (words.size() != spec->name.size())
    throw std::runtime_error(
        std::format("{} does not fit the usage `{}`", typed, spec->usage));
  _log.write(Level::debug, Tag::run, std::format("command: {}", typed));
  Run call(spec->command);
  spec->handler->command(call);
}

void Commands::source(const std::filesystem::path &file) {
  std::ifstream in(file);
  if (!in)
    throw std::runtime_error(std::format("{}: cannot be read", file.string()));
  std::size_t number = 0;
  for (std::string line; !_quitting && std::getline(in, line);) {
    ++number;
    // A logic_error is a bug in vulpen, not a mistake in the file, so it keeps its own
    // message.
    try {
      run(line);
    } catch (const std::runtime_error &failure) {
      throw std::runtime_error(
          std::format("{}:{}: {}", file.string(), number, failure.what()));
    }
  }
}

bool Commands::quitting() const {
  return _quitting;
}

void Commands::command(Call &call) {
  if (call.is(_quit)) {
    _quitting = true;
    _log.write(Level::info, Tag::run, "quit: the run ends before its next frame");
  }
}

// The longest name the line starts with, so a name may begin another (`view`,
// `view save`).
const Commands::Spec *Commands::match(std::span<const std::string_view> words) const {
  const Spec *found = nullptr;
  for (const Spec &spec : _specs)
    if (spec.name.size() <= words.size() &&
        std::ranges::equal(spec.name, words.first(spec.name.size())) &&
        (!found || spec.name.size() > found->name.size()))
      found = &spec;
  return found;
}

} // namespace VP
