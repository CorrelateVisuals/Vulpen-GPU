// The command port. It runs past 800 lines (CPP12): matching a line to a usage, its
// paths, the refusals, the log a session keeps and the port's own commands share the
// specs it holds, which one owner holds (A01).
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
#include <system_error>
#include <utility>

namespace VP {

namespace {

constexpr std::string_view blanks = " \t\r";

// The frames after a person's line in which the parts' lines answer it: the panes part
// answers at its next cook, the docking part at the one after.
constexpr std::uint64_t answer_frames = 3;
// The node words' kind, which a line starting with a word=value is refused naming.
constexpr std::string_view word_value{"word=value"};
// What an argument is, as the placeholder in a usage names it, and where completing it
// looks. A usage names only these, so a command declares its completion with its usage
// (RV04).
constexpr std::array kinds{
    std::string_view{"name"},       // a new name: nothing to complete
    std::string_view{"node"},       // the view's nodes
    std::string_view{"port"},       // node.port: the view's nodes, then their ports
    std::string_view{"connection"}, // the view's connections
    word_value,                     // the node words, then each word's values
    std::string_view{"key"},        // the node's params
    std::string_view{"value"},      // any word: nothing to complete
    std::string_view{"file"},       // the folders and files where the path resolves
    std::string_view{"view"},       // a view hosted, by its name, at any depth
    std::string_view{"project"},    // a project by name, or a folder by path, as a file
    std::string_view{"path"},       // a file the view holds by name, or any by path
    std::string_view{"folder"},     // a folder where the line counts from, or any by path
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

// Why a line's words name no command, which lists the commands they could have meant: a
// word=value, as a line going on from node set, those taking one; the words a group of
// commands starts with, as view alone, that group's; any other word, every command.
std::string unknown(std::span<const std::string_view> words,
                    std::span<const Usage> usages) {
  std::vector<std::string_view> meant;
  const auto starting = [&](std::size_t given) {
    for (const Usage &usage : usages)
      if (const std::vector<std::string_view> its = words_of(usage.usage);
          its.size() > given && std::ranges::equal(words.first(given),
                                                   std::span(its).first(given)))
        meant.push_back(usage.usage);
  };
  if (words.front().find('=') != std::string_view::npos) {
    const std::string kind = std::format("<{}>", word_value);
    for (const Usage &usage : usages)
      if (usage.usage.find(kind) != std::string_view::npos)
        meant.push_back(usage.usage);
    return std::format("unknown command {}: a word=value follows a command that takes "
                       "one, as {}",
                       words.front(),
                       joined(meant));
  }
  for (std::size_t given = words.size(); given > 0; --given) {
    starting(given);
    if (meant.empty())
      continue;
    std::ranges::sort(meant);
    return std::format("unknown command {}; the commands {} starts are {}",
                       joined(words, " "),
                       joined(words.first(given), " "),
                       joined(meant));
  }
  return std::format("unknown command {}; the commands are {}",
                     words.front(),
                     joined(std::views::transform(usages, &Usage::usage)));
}

// A lowercase letter, then lowercase letters, digits and -, as a new node's name is, so a
// command a node registers can start with its name.
bool command_word(std::string_view word) {
  const auto lower = [](char letter) { return letter >= 'a' && letter <= 'z'; };
  return !word.empty() && lower(word.front()) &&
         std::ranges::all_of(word, [&](char letter) {
           return lower(letter) || (letter >= '0' && letter <= '9') || letter == '-';
         });
}

// Whether the word is `<kind>` of a known kind.
bool placeholder(std::string_view word) {
  return word.size() > 2 && word.front() == '<' && word.back() == '>' &&
         std::ranges::find(kinds, word.substr(1, word.size() - 2)) != kinds.end();
}

// A path from a folder, an empty one being where vulpen started, and none the folder
// itself. Normal, as a replay names a file through .. from beside the log, and the view
// it hosts must be the one the same file named another way made.
std::filesystem::path resolved_from(const std::filesystem::path &folder,
                                    const std::filesystem::path &file) {
  const std::filesystem::path path = file.is_relative() ? folder / file : file;
  return std::filesystem::absolute(path.empty() ? "." : path).lexically_normal();
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
  void keep_log_to(std::span<const std::string> views) override {
    _port.keep_log_to(views);
  }
  std::string_view current() override {
    return _port.current();
  }
  std::string send_typed(std::string_view line, std::string_view folder) override {
    return _port.run(line, folder);
  }
  std::size_t done() const override {
    return _port.done();
  }

  Commands &_port;
};

class Run final : public Call {
public:
  // address: the name of the view the command addresses, which outlives the run; from:
  // where the line's relative paths count from.
  Run(Commands &port,
      ViewLookup *views,
      std::string_view address,
      FilePort &files,
      ProcessPort &processes,
      Command command,
      std::span<const std::string_view> arguments,
      std::filesystem::path from)
      : _port(port), _views(views), _address(address), _files(files),
        _processes(processes), _command(command), _arguments(arguments),
        _from(std::move(from)) {}
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
    return found(this->view(_address));
  }
  const View *view(std::string_view name) const override {
    return _views ? _views->find(name) : nullptr;
  }
  std::span<const Cost> costs() const override {
    return _views ? _views->costs(_address) : std::span<const Cost>();
  }
  std::span<const NodePort> ports() const override {
    return _views ? _views->ports(_address) : std::span<const NodePort>();
  }
  FilePort &files() override {
    return _files;
  }
  ProcessPort &processes() override {
    return _processes;
  }
  const View &unfolded() const override {
    return found(_views ? _views->unfolded(_address) : nullptr);
  }
  std::string resolved(std::string_view path) const override {
    return resolved_from(_from, path).string();
  }
  static const View &found(const View *view) {
    if (!view)
      throw std::runtime_error("no view to read yet");
    return *view;
  }

  Nested _port;
  ViewLookup *const _views;
  const std::string_view _address;
  FilePort &_files;
  ProcessPort &_processes;
  const Command _command;
  const std::span<const std::string_view> _arguments;
  const std::filesystem::path _from;
  std::string _reply;
};

} // namespace

void CommandLog::open() {
  _joined.reset();
  if (_groups.empty() || !_groups.back().empty()) {
    _groups.emplace_back();
    _taken.push_back(false);
  }
}

void CommandLog::join(std::size_t group) {
  _joined = group;
}

void CommandLog::keep(std::vector<std::string> words,
                      std::vector<std::size_t> files,
                      std::string_view view,
                      Primitive primitive) {
  _groups[_joined.value_or(_groups.size() - 1)].push_back(
      {std::move(words), std::move(files), std::string(view), primitive});
  ++_size;
}

std::string CommandLog::text(const std::filesystem::path &folder) const {
  std::string text;
  for (std::size_t group = 0; group < _groups.size(); ++group)
    for (const Kept &kept : _groups[group])
      if (!_taken[group])
        text += line(kept, folder);
  return text;
}

std::size_t CommandLog::size() const {
  return _size;
}

std::size_t CommandLog::group() const {
  return _joined.value_or(_groups.size() - 1);
}

void CommandLog::take_back(std::size_t group, bool taken) {
  _taken[group] = taken;
}

std::vector<std::string> CommandLog::views_since(std::size_t from) const {
  std::vector<std::string> views;
  std::size_t index = 0;
  for (const Kept &kept : _groups | std::views::join)
    if (index++ >= from && kept.primitive == Primitive::edit &&
        std::ranges::find(views, kept.view) == views.end())
      views.push_back(kept.view);
  return views;
}

// A save writes what came before it, so its file holds it, and a replay of the save
// would only write the file again.
std::string CommandLog::after(std::span<const Mark> marks,
                              const std::filesystem::path &folder) const {
  std::string text;
  std::size_t index = 0;
  for (std::size_t group = 0; group < _groups.size(); ++group)
    for (const Kept &kept : _groups[group]) {
      const auto mark = std::ranges::find(marks, kept.view, &Mark::view);
      if (mark != marks.end() && index >= mark->kept &&
          kept.primitive != Primitive::save && !_taken[group])
        text += line(kept, folder);
      ++index;
    }
  return text;
}

// A file on another drive than the log's has no relative path, so it stays absolute.
std::string CommandLog::line(const Kept &kept, const std::filesystem::path &folder) {
  std::string text;
  for (std::size_t index = 0; index < kept.words.size(); ++index) {
    std::filesystem::path word(kept.words[index]);
    if (std::ranges::find(kept.files, index) != kept.files.end())
      word = word.lexically_normal().lexically_proximate(folder.lexically_normal());
    text.append(index == 0 ? "" : " ").append(word.generic_string());
  }
  return text + '\n';
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
  std::optional<std::string> view; // whose node registered it; none for the engine's

  // A <project> that names no folder names a project, which its command finds, so only a
  // path resolves, and the log names it from its own folder; a <path> or a <folder> so
  // too, as its command finds a name where it is. A folder is named by either separator
  // the system knows, as Windows takes \ beside /.
  bool names_file(std::size_t argument, std::string_view word) const {
    const std::string_view kind =
        placeholders[std::min(argument, placeholders.size() - 1)];
    return kind == "file" ||
           ((kind == "project" || kind == "path" || kind == "folder") &&
            std::filesystem::path(word).has_parent_path());
  }

  // What a refusal adds for the words past the usage's last placeholder: which they
  // are, and after a path, that it holds no blank, as a path a blank split reads (A02).
  std::string too_many(std::span<const std::string_view> arguments) const {
    if (more || arguments.size() <= placeholders.size())
      return {};
    const std::span<const std::string_view> past = arguments.subspan(placeholders.size());
    const std::string_view last = placeholders.empty() ? "" : placeholders.back();
    return std::format(": {} {} {} too many{}",
                       joined(past, " "),
                       past.size() == 1 ? "is" : "are",
                       past.size() == 1 ? std::string("one word")
                                        : std::format("{} words", past.size()),
                       last == "file" || last == "path" || last == "folder"
                           ? ", as a path holds no blank"
                           : "");
  }
};

Commands::Commands(const Log &log, FilePort &files, ProcessPort &processes)
    : _log(log), _files(files), _processes(processes),
      _quit(add("quit",
                "ends the run before its next frame, unless a node holds work the end "
                "would lose, as a file's changes, or a project's files differ from its "
                "last view save",
                *this)),
      _quit_force(add("quit force",
                      "ends the run before its next frame, even while a node holds work "
                      "the end would lose, each project's files put back as its last "
                      "view save left them",
                      *this)),
      _source(add("source <file>",
                  "runs a file's commands, one a line, and stops at the first that "
                  "fails; its lines go to the view vulpen started with, but those "
                  "naming another, as `<view>: <command>`",
                  *this)),
      _log_save(add("log save <file>",
                    "writes the session's edits to a file, which source replays; a log "
                    "holds lines, not the files they made, so it replays where its "
                    "projects' files are",
                    *this)),
      _switch(add("view switch [<view>]",
                  "makes a hosted view the current one, where the lines typed go and "
                  "which every face shows; with none, the view vulpen started with",
                  *this)) {}

Commands::~Commands() = default;

Command Commands::add(std::string_view usage,
                      std::string_view help,
                      CommandHandler &handler,
                      Primitive primitive,
                      std::optional<std::string_view> view) {
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
  const auto arguments = std::ranges::find_if_not(words, command_word);
  if (arguments == words.begin() || ((more || optional) && arguments == words.end()) ||
      !std::all_of(arguments, words.end(), placeholder))
    throw std::runtime_error(std::format(
        "command `{}`: a usage is words of a lowercase letter, then lowercase "
        "letters, digits and -, then placeholders of the kinds {}",
        usage,
        joined(kinds)));
  const std::span<const std::string_view> name(words.begin(), arguments);
  if (std::ranges::any_of(_specs, [&](const Spec &spec) {
        return std::ranges::equal(spec.name, name) &&
               (!spec.view || !view || *spec.view == *view);
      }))
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
                    .primitive = primitive,
                    .view = view ? std::optional<std::string>(*view) : std::nullopt});
  return command;
}

void Commands::remove(Command command) {
  std::erase_if(_specs,
                [&](const Spec &spec) { return spec.command.index == command.index; });
}

std::string Commands::run(std::string_view line, std::string_view typed_at) {
  // A line from outside the port opens a group and addresses the view it names; a line
  // a command sends joins the group, and the view, of the line that runs the command.
  if (_depth == 0 && !std::exchange(_joining, false))
    _session.open();
  std::vector<std::string_view> words = words_of(line);
  const std::string address = view_named(words);
  if (words.empty())
    return {};
  const std::string_view typed(words.front().data(),
                               words.back().data() + words.back().size());
  const Spec *const spec = match(words, address);
  if (!spec)
    throw std::runtime_error(unknown(words, usages()));
  std::vector<std::string_view> arguments(words.begin() + spec->name.size(), words.end());
  const std::size_t expected = spec->placeholders.size();
  const std::size_t least = spec->optional ? expected - 1 : expected;
  if (arguments.size() < least || (arguments.size() > expected && !spec->more))
    throw std::runtime_error(std::format("{} does not fit the usage `{}`{}",
                                         typed,
                                         spec->usage,
                                         spec->too_many(arguments)));
  _log.write(Level::debug, Tag::run, std::format("command: {}", typed));
  std::filesystem::path from = paths_from(typed_at);
  const std::vector<std::string> paths = resolve(*spec, arguments, from);
  // Read first: a handler that rebuilds a view binds operators, which may register
  // commands and so move the specs.
  const Primitive primitive = spec->primitive;
  std::vector<std::string> kept(spec->name.begin(), spec->name.end());
  if (!address.empty())
    kept.insert(kept.begin(), address + ':');
  std::vector<std::size_t> files;
  for (std::size_t index = 0; index < arguments.size(); ++index) {
    if (spec->names_file(index, arguments[index]))
      files.push_back(kept.size());
    kept.emplace_back(arguments[index]);
  }
  const Restore depth(_depth, _depth + 1);
  const Restore addressed(_addressed, address);
  const Restore changed(_changed, address);
  Run call(*this,
           _views,
           address,
           this->files(),
           _processes,
           spec->command,
           arguments,
           std::move(from));
  spec->handler->command(call);
  if (primitive != Primitive::no && !_replaying && !_discarding)
    _session.keep(std::move(kept), std::move(files), _changed, primitive);
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
        std::format("no view is named {}; view list lists those a view hosts", name));
  return name;
}

// Here, as the line was typed or the script running it, so a command and the log name
// the same file (RP02). Returns where the paths are, reserved first, so adding one never
// moves the others.
std::vector<std::string> Commands::resolve(const Spec &spec,
                                           std::vector<std::string_view> &arguments,
                                           const std::filesystem::path &from) const {
  std::vector<std::string> paths;
  paths.reserve(arguments.size());
  for (std::size_t index = 0; index < arguments.size(); ++index)
    if (spec.names_file(index, arguments[index]))
      arguments[index] =
          paths.emplace_back(resolved_from(from, arguments[index]).string());
  return paths;
}

// Each once, though two views' nodes of one name hold it.
std::vector<Usage> Commands::usages() const {
  std::vector<Usage> usages;
  for (const Spec &spec : _specs)
    if (std::ranges::none_of(usages, [&](const Usage &usage) {
          return usage.usage == spec.usage;
        }))
      usages.push_back({spec.usage, spec.help});
  return usages;
}

std::span<const Logged> Commands::log() const {
  return std::span(_logged).first(_logged_count);
}

void Commands::keep_log_to(std::span<const std::string> views) {
  _log.keep_to(views);
}

// The nearest view that held it, once it is no longer hosted, as after view close of it
// or of a view holding it.
std::string_view Commands::current() {
  while (!_current.empty() && (!_views || !_views->find(_current))) {
    _current = _holding.empty() ? std::string() : std::move(_holding.front());
    if (!_holding.empty())
      _holding.erase(_holding.begin());
  }
  return _current;
}

// It changes no view, so the log keeps none of it, and every line it keeps names its
// view (V08).
void Commands::switch_to(std::string_view name) {
  if (!name.empty() && (!_views || !_views->find(name)))
    throw std::runtime_error(
        std::format("no view is named {}; view list lists those a view hosts", name));
  _current = name;
  _holding.clear();
  for (std::optional<std::string> host = _views ? _views->host_of(name) : std::nullopt;
       host && !host->empty();
       host = _views->host_of(*host))
    _holding.push_back(*host);
}

void Commands::frame() {
  ++_frames;
  _logged_count = _log.take(_logged);
  std::swap(_holds, _held);
  _holds.count = 0;
}

void Commands::hold(std::string_view node, std::string_view what) {
  if (_holds.count == _holds.lines.size())
    _holds.lines.emplace_back();
  _holds.lines[_holds.count++].assign(node).append(": ").append(what);
}

// This frame's holds, then those of the frame before it has not repeated yet, as a
// node's that cooks after the line quitting; each once.
std::vector<std::string_view> Commands::holding() const {
  std::vector<std::string_view> held;
  for (const Holds *const holds : {&_holds, &_held})
    for (const std::string &line : std::span(holds->lines).first(holds->count))
      if (std::ranges::find(held, line) == held.end())
        held.emplace_back(line);
  return held;
}

std::string Commands::send(std::string_view line) {
  return answer(line, {}, false);
}

std::string Commands::send_typed(std::string_view line, std::string_view folder) {
  return answer(line, folder, true);
}

// A line a person typed, at a terminal or through a menu, writes each project it changed
// at once, refused or not, as a refused line may have run edits before the one refused,
// and so do the lines of a script it sources; a line a node sends writes none, as it may
// come every frame, nor does a script vulpen starts with, which replays a session or
// sets a run up, as a restart's does (V08).
std::string Commands::answer(std::string_view line, std::string_view folder, bool typed) {
  const std::size_t from = _session.size();
  if (typed)
    _log.apart();
  // A step of its own, refused or not, as a refused line may have run edits before the
  // one refused; a line a step's line runs is part of that step.
  const bool steps = typed && _depth == 0 && _views;
  if (steps)
    _views->begin_step(line);
  // A part's line of the frame a person's line ran, or of the few after it, as a part
  // answers at its next cook and another answers that, is part of what that line did.
  const bool answers =
      !typed && _depth == 0 && _frames - _answered_frame <= answer_frames;
  _joining = answers && !_answers_undo && _answered;
  if (_joining)
    _session.join(*_answered);
  const Restore discarding(_discarding, _discarding || (answers && _answers_undo));
  const auto end_step = [&] {
    if (steps && _session.size() > from) {
      _answered = _session.group();
      _answered_frame = _frames;
      _answers_undo = false;
    }
    if (steps)
      _views->end_step(_session.size() > from ? std::optional(_session.group())
                                              : std::nullopt);
  };
  try {
    std::string answered = run(line, folder);
    if (typed)
      write_since(from);
    end_step();
    return answered;
  } catch (const std::runtime_error &failure) {
    if (typed)
      write_since(from);
    end_step();
    _log.write(Level::error, Tag::run, failure.what());
    return {};
  }
}

std::size_t Commands::done() const {
  return _views ? _views->done() : 0;
}

// A line taken back hides the answers that joined it, and a redo brings them back, so
// the log replays to what runs (V08); the parts' answers to the undo or the redo itself
// are kept nowhere.
void Commands::take_back(std::size_t group, bool taken) {
  _session.take_back(group, taken);
  _answered.reset();
  _answered_frame = _frames;
  _answers_undo = true;
}

void Commands::write_since(std::size_t from) {
  if (_views)
    for (const std::string &view : _session.views_since(from))
      _views->write(view);
}

void Commands::source(const std::filesystem::path &file, bool past_refusals) {
  // A name the system cannot even look up, as one too long for it, is unreadable too.
  std::error_code unread;
  const std::filesystem::path path =
      std::filesystem::weakly_canonical(resolved(file), unread);
  if (unread)
    throw std::runtime_error(std::format(
        "{}: cannot be read: {}", resolved(file).string(), unread.message()));
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
  std::vector<std::string> refused; // the views whose lines past_refusals passes over
  for (std::string line; !_quitting && std::getline(in, line);) {
    _sourcing.back().line = ++number;
    // The view the line names, as written, which a refusal may be that it is none.
    const std::vector<std::string_view> words = words_of(line);
    const std::string address = !words.empty() && words.front().ends_with(':')
                                    ? std::string(words.front().substr(
                                          0, words.front().size() - 1))
                                    : std::string();
    if (std::ranges::find(refused, address) != refused.end())
      continue;
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
      if (past_refusals) {
        refused.push_back(address);
        _log.write(Level::error,
                   Tag::run,
                   std::format("{}:{}: {}; the lines after it of {} are passed over",
                               path.string(),
                               number,
                               message,
                               address.empty() ? "the view vulpen started with"
                                               : address));
        continue;
      }
      _sourcing.pop_back();
      throw std::runtime_error(std::format("{}:{}: {}", path.string(), number, message));
    }
  }
  _sourcing.pop_back();
}

std::vector<std::string> Commands::replay(std::string_view lines) {
  const Restore replaying(_replaying, true);
  std::vector<std::string> refused;
  for (const auto piece : std::views::split(lines, '\n'))
    if (const std::string_view line(piece.begin(), piece.end()); !line.empty())
      try {
        run(line);
      } catch (const std::runtime_error &failure) {
        refused.push_back(std::format("{}: {}", line, failure.what()));
      }
  return refused;
}

bool Commands::replaying() const {
  return _replaying;
}

void Commands::look_in(ViewLookup &views) {
  _views = &views;
}

FilePort &Commands::files() {
  return _views ? _views->files() : _files;
}

const View *Commands::view(std::string_view name) {
  return _views ? _views->find(name) : nullptr;
}

const View *Commands::unfolded(std::string_view name) {
  return _views ? _views->unfolded(name) : nullptr;
}

std::span<const Cost> Commands::costs(std::string_view name) {
  return _views ? _views->costs(name) : std::span<const Cost>();
}

std::span<const NodePort> Commands::ports(std::string_view name) {
  return _views ? _views->ports(name) : std::span<const NodePort>();
}

bool Commands::quitting() const {
  return _quitting;
}

const CommandLog &Commands::session() const {
  return _session;
}

void Commands::changes(std::string_view view) {
  _changed = view;
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
  if (call.is(_quit) || call.is(_quit_force)) {
    quit(call.is(_quit_force));
  } else if (call.is(_source)) {
    source(call.arguments().front());
  } else if (call.is(_switch)) {
    switch_to(call.arguments().empty() ? std::string_view() : call.arguments().front());
    call.reply(_current.empty()
                   ? std::string("lines go to the view vulpen started with now")
                   : std::format("lines go to {} now", _current));
  } else if (call.is(_log_save)) {
    const std::string_view file = call.arguments().front();
    std::error_code unread;
    if (std::filesystem::is_directory(file, unread))
      throw std::runtime_error(
          std::format("{} is a folder; log save writes a file", file));
    Files::save(file, _session.text(std::filesystem::path(file).parent_path()));
    _log.write(Level::info, Tag::run, std::format("log save: {}", file));
  }
}

// The run ends before its next frame, unless quit finds what the end would lose, each
// named: work a node holds, and a project's files no view save kept (1c). quit force
// ends it anyway, each such project's files put back as its last view save left them.
void Commands::quit(bool force) {
  const std::vector<std::string_view> held = holding();
  std::vector<std::string> lost(held.begin(), held.end());
  std::vector<std::string> unkept; // the projects whose files no view save kept
  for (const std::string &view : _views ? _views->names() : std::vector<std::string>())
    if (const std::vector<std::string> files = _views->unkept(view); !files.empty()) {
      unkept.push_back(view);
      lost.push_back(std::format(
          "{}: {}", view.empty() ? "the view vulpen started with" : view, joined(files)));
    }
  if (!force && !lost.empty())
    throw std::runtime_error(
        std::format("quit would lose what nodes hold, or what no view save kept, so the "
                    "run goes on; view save keeps a project, quit force ends it anyway, "
                    "a project's files put back as its last view save left them:\n{}",
                    joined(lost, "\n")));
  for (const std::string &view : unkept)
    try {
      _views->put_back(view);
    } catch (const std::runtime_error &failure) {
      _log.write(Level::error, Tag::run, failure.what());
    }
  _quitting = true;
  _log.write(Level::info, Tag::run, "quit: the run ends before its next frame");
}

// The longest name the line starts with, so a name may begin another (`view`,
// `view save`).
// The longest a line's words start with: of those, the one of the view the line
// addresses, else the only one; several views' with none of them addressed are refused,
// naming the views, as a line runs one view's.
const Commands::Spec *Commands::match(std::span<const std::string_view> words,
                                      std::string_view address) const {
  std::vector<const Spec *> found;
  for (const Spec &spec : _specs)
    if (spec.name.size() <= words.size() &&
        std::ranges::equal(spec.name, words.first(spec.name.size()))) {
      if (!found.empty() && spec.name.size() > found.front()->name.size())
        found.clear();
      if (found.empty() || spec.name.size() == found.front()->name.size())
        found.push_back(&spec);
    }
  if (found.size() < 2)
    return found.empty() ? nullptr : found.front();
  const auto own = std::ranges::find_if(found, [&](const Spec *spec) {
    return spec->view && *spec->view == address;
  });
  if (own != found.end())
    return *own;
  // Both string_views, as a std::string the two would make dies with the line.
  std::vector<std::string_view> views;
  for (const Spec *const spec : found)
    views.push_back(spec->view->empty() ? std::string_view("the view vulpen started with")
                                        : std::string_view(*spec->view));
  throw std::runtime_error(std::format("{} is a command of {}: a line names its view, as "
                                       "`{}: {}`",
                                       joined(found.front()->name, " "),
                                       joined(views),
                                       *found.front()->view,
                                       found.front()->usage));
}

// A relative path names a file beside the one running (RP02), so a script and the files
// it reads and writes move together; in a line typed, in the folder it was typed at, as
// its prompt shows it, as in a shell; in any other, from where vulpen started.
std::filesystem::path Commands::paths_from(std::string_view typed_at) const {
  return _sourcing.empty() ? std::filesystem::path(typed_at)
                           : _sourcing.back().file.parent_path();
}

std::filesystem::path Commands::resolved(const std::filesystem::path &file) const {
  return resolved_from(paths_from({}), file);
}

} // namespace VP
