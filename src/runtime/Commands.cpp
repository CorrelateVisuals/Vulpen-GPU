#include "runtime/Commands.h"

#include "baseclasses/Platform.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <string>

namespace VP {

namespace {

constexpr std::string_view blanks = " \t\r";

// What an argument is, as the placeholder in a usage names it, and where completing it
// looks. A usage names only these, so a command declares its completion with its usage
// (RV04).
constexpr std::array kinds{
    std::string_view{"name"},       // a new name: nothing to complete
    std::string_view{"node"},       // the view's nodes
    std::string_view{"port"},       // node.port: the view's nodes, then their ports
    std::string_view{"connection"}, // the view's connections
    std::string_view{"word=value"}, // the node words, then each word's values
    std::string_view{"key"},        // the node's params
    std::string_view{"value"},      // any word: nothing to complete
    std::string_view{"file"},       // the folders and files where the path resolves
    std::string_view{"recipe"},     // the folders of the view's recipes
    std::string_view{"deploy"},     // the view's deploys
};
// After the last placeholder: one argument or more.
constexpr std::string_view one_or_more = "...";

std::vector<std::string_view> words_of(std::string_view line) {
  std::vector<std::string_view> words;
  for (std::size_t at = line.find_first_not_of(blanks); at != std::string_view::npos;) {
    const std::size_t end = std::min(line.find_first_of(blanks, at), line.size());
    words.push_back(line.substr(at, end - at));
    at = line.find_first_not_of(blanks, end);
  }
  return words;
}

// Names one after another, as an error lists them, or words as the log keeps them.
template <class Names>
std::string joined(const Names &names, std::string_view between = ", ") {
  std::string text;
  for (const std::string_view name : names)
    text.append(text.empty() ? "" : between).append(name);
  return text;
}

bool lowercase(std::string_view word) {
  return !word.empty() && std::ranges::all_of(word, [](char letter) {
    return letter >= 'a' && letter <= 'z';
  });
}

// Whether the word is `<kind>` of a known kind.
bool placeholder(std::string_view word) {
  return word.size() > 2 && word.front() == '<' && word.back() == '>' &&
         std::ranges::find(kinds, word.substr(1, word.size() - 2)) != kinds.end();
}

class Run final : public Call {
public:
  Run(Command command, std::span<const std::string_view> arguments)
      : _command(command), _arguments(arguments) {}

private:
  bool is(Command command) const override {
    return command.index == _command.index;
  }
  std::span<const std::string_view> arguments() const override {
    return _arguments;
  }

  const Command _command;
  const std::span<const std::string_view> _arguments;
};

} // namespace

void CommandLog::open() {
  if (_groups.empty() || !_groups.back().empty())
    _groups.emplace_back();
}

void CommandLog::keep(std::string_view primitive) {
  _groups.back().emplace_back(primitive);
}

std::string CommandLog::text() const {
  std::string text;
  for (const std::string &primitive : _groups | std::views::join)
    text.append(primitive).push_back('\n');
  return text;
}

struct Commands::Spec {
  std::vector<std::string> name; // the words a line starts with
  std::size_t arguments = 0;     // one per placeholder
  bool more = false;             // the last placeholder takes one argument or more
  std::string usage;
  std::string help;
  Command command;
  CommandHandler *handler = nullptr;
  Primitive primitive = Primitive::no;
};

Commands::Commands(const Log &log)
    : _log(log), _quit(add("quit", "ends the run before its next frame", *this)),
      _source(add("source <file>",
                  "runs a file's commands, one a line, and stops at the first that fails",
                  *this)),
      _log_save(add("log save <file>",
                    "writes the session's edits to a file, which source replays",
                    *this)) {}

Commands::~Commands() = default;

Command Commands::add(std::string_view usage,
                      std::string_view help,
                      CommandHandler &handler,
                      Primitive primitive) {
  std::vector<std::string_view> words = words_of(usage);
  if (words.empty() || help.empty())
    throw std::runtime_error(
        std::format("command `{}` needs a usage and a help to register (RV04)", usage));
  const bool more = words.back().ends_with(one_or_more);
  if (more)
    words.back().remove_suffix(one_or_more.size());
  const auto arguments = std::ranges::find_if_not(words, lowercase);
  if (arguments == words.begin() || (more && arguments == words.end()) ||
      !std::all_of(arguments, words.end(), placeholder))
    throw std::runtime_error(std::format(
        "command `{}`: a usage is lowercase words, then placeholders of the kinds {}",
        usage,
        joined(kinds)));
  const std::span<const std::string_view> name(words.begin(), arguments);
  if (const Spec *const twin = match(name); twin && twin->name.size() == name.size())
    throw std::runtime_error(std::format("command `{}` registers twice", usage));
  // From 1, so a Command no add returned matches none.
  const Command command{++_added};
  _specs.push_back({.name = {name.begin(), name.end()},
                    .arguments = static_cast<std::size_t>(words.end() - arguments),
                    .more = more,
                    .usage = std::string(usage),
                    .help = std::string(help),
                    .command = command,
                    .handler = &handler,
                    .primitive = primitive});
  return command;
}

void Commands::remove(Command command) {
  std::erase_if(_specs,
                [&](const Spec &spec) { return spec.command.index == command.index; });
}

void Commands::run(std::string_view line) {
  _session.open();
  const std::vector<std::string_view> words = words_of(line);
  if (words.empty())
    return;
  const std::string_view typed(words.front().data(),
                               words.back().data() + words.back().size());
  const Spec *const spec = match(words);
  if (!spec)
    throw std::runtime_error(
        std::format("unknown command {}; the commands are {}",
                    words.front(),
                    joined(std::views::transform(_specs, &Spec::usage))));
  const std::span<const std::string_view> arguments =
      std::span(words).subspan(spec->name.size());
  if (arguments.size() < spec->arguments ||
      (arguments.size() > spec->arguments && !spec->more))
    throw std::runtime_error(
        std::format("{} does not fit the usage `{}`", typed, spec->usage));
  _log.write(Level::debug, Tag::run, std::format("command: {}", typed));
  Run call(spec->command, arguments);
  // Read first: a handler that rebuilds a view binds operators, which may register
  // commands and so move the specs.
  const Primitive primitive = spec->primitive;
  spec->handler->command(call);
  if (primitive == Primitive::yes)
    _session.keep(joined(words, " "));
}

void Commands::source(const std::filesystem::path &file) {
  const std::filesystem::path path = std::filesystem::weakly_canonical(resolved(file));
  if (std::ranges::find(_sourcing, path, &Source::file) != _sourcing.end())
    throw std::runtime_error(std::format(
        "{} is running already, so sourcing it again would never end", path.string()));
  std::ifstream in(path);
  if (!in || !std::filesystem::is_regular_file(path))
    throw std::runtime_error(std::format("{}: cannot be read", path.string()));
  _sourcing.push_back({path});
  std::size_t number = 0;
  for (std::string line; !_quitting && std::getline(in, line);) {
    _sourcing.back().line = ++number;
    // A logic_error is a bug in vulpen, not a mistake in the file, so it keeps its own
    // message, and ends the run.
    try {
      run(line);
    } catch (const std::runtime_error &failure) {
      // An error that names this line already, as a deploy's does, names it once.
      std::string_view message = failure.what();
      if (const std::string here = where() + ": "; message.starts_with(here))
        message.remove_prefix(here.size());
      _sourcing.pop_back();
      throw std::runtime_error(std::format("{}:{}: {}", path.string(), number, message));
    }
  }
  _sourcing.pop_back();
}

bool Commands::quitting() const {
  return _quitting;
}

std::string Commands::where() const {
  if (_sourcing.empty())
    return {};
  return std::format(
      "{}:{}", _sourcing.back().file.filename().string(), _sourcing.back().line);
}

void Commands::command(Call &call) {
  if (call.is(_quit)) {
    _quitting = true;
    _log.write(Level::info, Tag::run, "quit: the run ends before its next frame");
  } else if (call.is(_source)) {
    source(call.arguments().front());
  } else if (call.is(_log_save)) {
    const std::filesystem::path file = resolved(call.arguments().front());
    Files::save(file, _session.text());
    _log.write(Level::info, Tag::run, std::format("log save: {}", file.string()));
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

// A relative path names a file beside the one running (RP02), so a script and the files
// it reads and writes move together.
std::filesystem::path Commands::resolved(const std::filesystem::path &file) const {
  return file.is_relative() && !_sourcing.empty()
             ? _sourcing.back().file.parent_path() / file
             : file;
}

} // namespace VP
