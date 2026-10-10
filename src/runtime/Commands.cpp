#include "runtime/Commands.h"

#include "baseclasses/Platform.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <format>
#include <fstream>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

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
    std::string_view{"recipe"},     // the recipes of the library
};
// After the last placeholder: one argument or more.
constexpr std::string_view one_or_more = "...";
// Around the last placeholder: it may be left out, so [<kind>...] takes none or more.
constexpr char optional_open = '[';
constexpr char optional_close = ']';

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

// Puts a value back when the scope ends, however it ends.
template <class T> class Restore {
public:
  Restore(T &slot, T value)
      : _slot(slot), _saved(std::exchange(slot, std::move(value))) {}
  ~Restore() {
    _slot = std::move(_saved);
  }
  Restore(const Restore &) = delete;
  Restore &operator=(const Restore &) = delete;

private:
  T &_slot;
  T _saved;
};

// The port as a command sends lines through it: a refusal fails the command too, so a
// script stops at the line that ran it, not after.
class Nested final : public CommandPort {
public:
  explicit Nested(Commands &port) : _port(port) {}

private:
  std::string send(std::string_view line) override {
    return _port.run(line);
  }
  std::vector<Usage> usages() const override {
    return _port.usages();
  }
  std::span<const Logged> log() const override {
    return _port.log();
  }

  Commands &_port;
};

class Run final : public Call {
public:
  // address: the name of the view the command addresses, which outlives the run.
  Run(Commands &port,
      ViewLookup *views,
      std::string_view address,
      FilePort &files,
      Command command,
      std::span<const std::string_view> arguments)
      : _port(port), _views(views), _address(address), _files(files), _command(command),
        _arguments(arguments) {}
  // Every line the command answered.
  std::string answer() && {
    return std::move(_reply);
  }

private:
  bool is(Command command) const override {
    return command.index == _command.index;
  }
  std::span<const std::string_view> arguments() const override {
    return _arguments;
  }
  void reply(std::string_view text) override {
    _reply.append(text);
    if (!text.ends_with('\n'))
      _reply.push_back('\n');
  }
  CommandPort &commands() override {
    return _port;
  }
  // Found anew each time, since a command this one sends may replace the view.
  const View &view() const override {
    const View *const view = _views ? _views->find(_address) : nullptr;
    if (!view)
      throw std::runtime_error("no view to read yet");
    return *view;
  }
  FilePort &files() override {
    return _files;
  }

  Nested _port;
  ViewLookup *const _views;
  const std::string_view _address;
  FilePort &_files;
  const Command _command;
  const std::span<const std::string_view> _arguments;
  std::string _reply;
};

} // namespace

void CommandLog::open() {
  if (_groups.empty() || !_groups.back().empty())
    _groups.emplace_back();
}

void CommandLog::keep(std::vector<std::string> words, std::vector<std::size_t> files) {
  _groups.back().push_back({std::move(words), std::move(files)});
}

// A file on another drive than the log's has no relative path, so it stays absolute.
std::string CommandLog::text(const std::filesystem::path &folder) const {
  std::string text;
  for (const Kept &kept : _groups | std::views::join) {
    for (std::size_t index = 0; index < kept.words.size(); ++index) {
      std::filesystem::path word(kept.words[index]);
      if (std::ranges::find(kept.files, index) != kept.files.end())
        word = word.lexically_normal().lexically_proximate(folder.lexically_normal());
      text.append(index == 0 ? "" : " ").append(word.generic_string());
    }
    text.push_back('\n');
  }
  return text;
}

struct Commands::Spec {
  std::vector<std::string> name; // the words a line starts with
  // The kind of each argument, from kinds; the last placeholder's for every one after
  // it, when it takes one argument or more.
  std::vector<std::string_view> placeholders;
  bool more = false;
  bool optional = false; // whether the last placeholder may be left out
  std::string usage;
  std::string help;
  Command command;
  CommandHandler *handler = nullptr;
  Primitive primitive = Primitive::no;

  bool names_file(std::size_t argument) const {
    return placeholders[std::min(argument, placeholders.size() - 1)] == "file";
  }
};

Commands::Commands(const Log &log, FilePort &files)
    : _log(log), _files(files),
      _quit(add("quit", "ends the run before its next frame", *this)),
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
  const bool optional = words.size() > 1 && words.back().front() == optional_open &&
                        words.back().back() == optional_close;
  if (optional)
    words.back() = words.back().substr(1, words.back().size() - 2);
  const bool more = words.back().ends_with(one_or_more);
  if (more)
    words.back().remove_suffix(one_or_more.size());
  const auto arguments = std::ranges::find_if_not(words, lowercase);
  if (arguments == words.begin() || ((more || optional) && arguments == words.end()) ||
      !std::all_of(arguments, words.end(), placeholder))
    throw std::runtime_error(std::format(
        "command `{}`: a usage is lowercase words, then placeholders of the kinds {}",
        usage,
        joined(kinds)));
  const std::span<const std::string_view> name(words.begin(), arguments);
  if (const Spec *const twin = match(name); twin && twin->name.size() == name.size())
    throw std::runtime_error(std::format("command `{}` registers twice", usage));
  std::vector<std::string_view> placeholders;
  for (auto word = arguments; word != words.end(); ++word)
    placeholders.push_back(*std::ranges::find(kinds, word->substr(1, word->size() - 2)));
  // From 1, so a Command no add returned matches none.
  const Command command{++_added};
  _specs.push_back({.name = {name.begin(), name.end()},
                    .placeholders = std::move(placeholders),
                    .more = more,
                    .optional = optional,
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

std::string Commands::run(std::string_view line) {
  // A line from outside the port opens a group and addresses the view it names; a line
  // a command sends joins the group, and the view, of the line that runs the command.
  if (_depth == 0)
    _session.open();
  std::vector<std::string_view> words = words_of(line);
  const std::string address = view_named(words);
  if (words.empty())
    return {};
  const std::string_view typed(words.front().data(),
                               words.back().data() + words.back().size());
  const Spec *const spec = match(words);
  if (!spec)
    throw std::runtime_error(
        std::format("unknown command {}; the commands are {}",
                    words.front(),
                    joined(std::views::transform(_specs, &Spec::usage))));
  std::vector<std::string_view> arguments(words.begin() + spec->name.size(), words.end());
  const std::size_t expected = spec->placeholders.size();
  const std::size_t least = spec->optional ? expected - 1 : expected;
  if (arguments.size() < least || (arguments.size() > expected && !spec->more))
    throw std::runtime_error(
        std::format("{} does not fit the usage `{}`", typed, spec->usage));
  _log.write(Level::debug, Tag::run, std::format("command: {}", typed));
  const std::vector<std::string> paths = resolve(*spec, arguments);
  // Read first: a handler that rebuilds a view binds operators, which may register
  // commands and so move the specs.
  const Primitive primitive = spec->primitive;
  std::vector<std::string> kept(spec->name.begin(), spec->name.end());
  if (!address.empty())
    kept.insert(kept.begin(), address + ':');
  std::vector<std::size_t> files;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (spec->names_file(index))
      files.push_back(kept.size());
    kept.emplace_back(arguments[index]);
  }
  const Restore depth(_depth, _depth + 1);
  const Restore addressed(_addressed, address);
  Run call(*this, _views, address, _files, spec->command, arguments);
  spec->handler->command(call);
  if (primitive == Primitive::yes)
    _session.keep(std::move(kept), std::move(files));
  return std::move(call).answer();
}

// The view a line addresses, whose name it takes off the line's words.
std::string Commands::view_named(std::vector<std::string_view> &words) const {
  if (words.empty() || !words.front().ends_with(':'))
    return _depth == 0 ? std::string() : _addressed;
  std::string name(words.front().substr(0, words.front().size() - 1));
  words.erase(words.begin());
  if (!name.empty() && (!_views || !_views->find(name)))
    throw std::runtime_error(
        std::format("no view is named {}; child list lists the views", name));
  return name;
}

// Here, beside the file running, so a command and the log name the same file (RP02).
// Returns where the paths are, reserved first, so adding one never moves the others.
std::vector<std::string>
Commands::resolve(const Spec &spec, std::vector<std::string_view> &arguments) const {
  std::vector<std::string> paths;
  paths.reserve(arguments.size());
  for (std::size_t index = 0; index < arguments.size(); ++index)
    if (spec.names_file(index))
      arguments[index] = paths.emplace_back(resolved(arguments[index]).string());
  return paths;
}

std::vector<Usage> Commands::usages() const {
  std::vector<Usage> usages;
  for (const Spec &spec : _specs)
    usages.push_back({spec.usage, spec.help});
  return usages;
}

std::span<const Logged> Commands::log() const {
  return std::span(_logged).first(_logged_count);
}

void Commands::frame() {
  _logged_count = _log.take(_logged);
}

std::string Commands::send(std::string_view line) {
  try {
    return run(line);
  } catch (const std::runtime_error &failure) {
    _log.write(Level::error, Tag::run, failure.what());
    return {};
  }
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
  // Each line stands alone, as a log's must (V08): a group of its own, addressing the
  // view it names.
  const Restore depth(_depth, std::size_t{0});
  const Restore addressed(_addressed, std::string());
  std::size_t number = 0;
  for (std::string line; !_quitting && std::getline(in, line);) {
    _sourcing.back().line = ++number;
    // A logic_error is a bug in vulpen, not a mistake in the file, so it keeps its own
    // message, and ends the run.
    try {
      const std::string reply = run(line);
      std::fwrite(reply.data(), 1, reply.size(), stdout);
      std::fflush(stdout);
    } catch (const std::runtime_error &failure) {
      // An error that names this line already, as one about a node it added does,
      // names it once.
      std::string_view message = failure.what();
      if (const std::string here = where() + ": "; message.starts_with(here))
        message.remove_prefix(here.size());
      _sourcing.pop_back();
      throw std::runtime_error(std::format("{}:{}: {}", path.string(), number, message));
    }
  }
  _sourcing.pop_back();
}

void Commands::look_in(ViewLookup &views) {
  _views = &views;
}

const View *Commands::view(std::string_view name) {
  return _views ? _views->find(name) : nullptr;
}

bool Commands::quitting() const {
  return _quitting;
}

std::string_view Commands::addressed() const {
  return _addressed;
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
    const std::string_view file = call.arguments().front();
    Files::save(file, _session.text(std::filesystem::path(file).parent_path()));
    _log.write(Level::info, Tag::run, std::format("log save: {}", file));
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
// it reads and writes move together; in a line typed, beside where it was typed.
std::filesystem::path Commands::resolved(const std::filesystem::path &file) const {
  return std::filesystem::absolute(file.is_relative() && !_sourcing.empty()
                                       ? _sourcing.back().file.parent_path() / file
                                       : file);
}

} // namespace VP
