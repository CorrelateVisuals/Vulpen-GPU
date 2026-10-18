#include "runtime/Live.h"

#include "baseclasses/Log.h"
#include "baseclasses/Platform.h"
#include "runtime/Commands.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace VP {

namespace {

constexpr auto scan_interval = std::chrono::milliseconds(100);
constexpr const char *build_log = "live-build.log";
constexpr const char *restart_log = "restart-build.log";
// In the build folder, which every build of the tree takes first, run.sh's and run.ps1's
// too.
constexpr const char *build_lock = "build.lock";

// When the file was last written; the earliest time there is while it is not there.
std::filesystem::file_time_type written(const std::filesystem::path &file) {
  std::error_code gone;
  const auto time = std::filesystem::last_write_time(file, gone);
  return gone ? std::filesystem::file_time_type::min() : time;
}

// A file's lines that hold anything, as paths.
std::vector<std::filesystem::path> lines_of(const std::filesystem::path &file) {
  std::vector<std::filesystem::path> lines;
  const std::string text = Files::text(file);
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    if (end > at)
      lines.emplace_back(text.substr(at, end - at));
    at = end + 1;
  }
  return lines;
}

// A build of the tree off the frame thread, which takes the tree's build.lock first.
class Builder {
public:
  // target: what it builds; log: the file in the build folder what it says goes to.
  Builder(const std::filesystem::path &build, std::string_view target, std::string_view log)
      : _log_file(build / log), _lock_file(build / build_lock),
        _command(std::format(R"("{}" --build "{}" --target {} --parallel > "{}" 2>&1)",
                             VP_CMAKE_COMMAND,
                             build.string(),
                             target,
                             _log_file.string())) {}

  void start() {
    _running = true;
    _started = std::chrono::steady_clock::now();
    _failure.clear();
    _thread = std::jthread([this] {
      // A build that cannot take the lock fails, naming why, rather than ending the run
      // as an exception leaving a thread would.
      try {
        const FileLock lock(_lock_file);
        _status = Shell::run(_command);
      } catch (const std::exception &failure) {
        _failure = failure.what();
        _status = -1;
      }
      _finished = true;
      _running = false;
    });
  }
  bool running() const {
    return _running;
  }
  // Once, as it ends: whether it built.
  std::optional<bool> done() {
    if (!_finished.exchange(false))
      return std::nullopt;
    return _status == 0;
  }
  std::string said() const {
    return _failure.empty() ? Files::text(_log_file) : _failure;
  }
  std::chrono::duration<double> took() const {
    return std::chrono::steady_clock::now() - _started;
  }

private:
  const std::filesystem::path _log_file;
  const std::filesystem::path _lock_file;
  const std::string _command;
  std::chrono::steady_clock::time_point _started{};
  std::atomic<bool> _running{false};
  std::atomic<bool> _finished{false};
  std::atomic<int> _status{0};
  std::string _failure; // the thread's, read once _finished says it is done
  std::jthread _thread;
};

} // namespace

struct Live::Scan {
  Scan(std::vector<std::filesystem::path> watched, const std::filesystem::path &build)
      : folders(std::move(watched)), stamp_file(build / VP_MODULE_STAMP_FILE),
        builder(build, "vulpen_modules", build_log),
        engine(lines_of(build / VP_ENGINE_SOURCES_FILE)),
        engine_said(written(Files::executable())), built_from(newest()),
        pending(built_from) {}

  // A name that starts with . is no node's and builds nothing, as the loader and the
  // build skip it, so what a part keeps there, as the graph its layout, starts no build.
  std::filesystem::file_time_type newest() const {
    // Not file_time_type{}: libstdc++ puts the file clock's epoch in the year 2174.
    auto newest = std::filesystem::file_time_type::min();
    std::error_code gone; // a file an editor is replacing may vanish mid-scan
    for (const std::filesystem::path &folder : folders)
      for (auto entry = std::filesystem::recursive_directory_iterator(folder, gone);
           entry != std::filesystem::recursive_directory_iterator();
           entry.increment(gone)) {
        if (entry->path().filename().string().starts_with('.')) {
          entry.disable_recursion_pending();
          continue;
        }
        newest = std::max(newest, entry->last_write_time(gone));
      }
    return newest;
  }

  // Once a scan, as a build reads none of them.
  void notice_engine() {
    for (const std::filesystem::path &file : engine)
      if (const auto time = written(file); time > engine_said) {
        engine_said = time;
        engine_change = file;
      }
  }

  std::vector<std::filesystem::path> folders;
  const std::filesystem::path stamp_file;
  Builder builder;
  const std::vector<std::filesystem::path> engine; // as the build lists them
  std::filesystem::file_time_type engine_said; // the newest engine file's, once said
  std::filesystem::path engine_change;
  std::filesystem::file_time_type built_from;
  std::filesystem::file_time_type pending;
  std::chrono::steady_clock::time_point next_scan{};
};

Live::Live(std::vector<std::filesystem::path> folders, const std::filesystem::path &build)
    : _scan(std::make_unique<Scan>(std::move(folders), build)) {}

Live::~Live() = default;

Live::Build Live::poll() {
  Scan &scan = *_scan;
  const auto now = std::chrono::steady_clock::now();
  Build build = Build::none;
  if (now >= scan.next_scan && !scan.builder.running()) {
    scan.next_scan = now + scan_interval;
    scan.notice_engine();
    const auto seen = scan.newest();
    if (seen != scan.built_from && seen == scan.pending) {
      scan.built_from = seen;
      scan.builder.start();
      build = Build::started;
    }
    scan.pending = seen;
  }
  if (const std::optional<bool> built = scan.builder.done())
    build = *built ? Build::succeeded : Build::failed;
  return build;
}

bool Live::loads() const {
  const std::string stamp = Files::text(_scan->stamp_file);
  return stamp.substr(0, stamp.find_last_not_of(" \r\n") + 1) == VP_MODULE_STAMP;
}

std::filesystem::path Live::engine_changed() {
  return std::exchange(_scan->engine_change, {});
}

bool Live::building() const {
  return _scan->builder.running();
}

std::string Live::said() const {
  return _scan->builder.said();
}

std::chrono::duration<double> Live::took() const {
  return _scan->builder.took();
}

void Live::watch(std::vector<std::filesystem::path> folders) {
  _scan->folders = std::move(folders);
}

struct Restart::State {
  State(Commands &port, const Log &said, const std::filesystem::path &build)
      : commands(port), log(said), builder(build, "vulpen", restart_log) {}

  Commands &commands;
  const Log &log;
  Builder builder;
  Command command;
  bool asked = false;
  bool building = false;
};

namespace {

std::string refusal(const std::vector<std::string_view> &held) {
  std::string text = "quit restart would lose what nodes hold, so the run goes on:";
  for (const std::string_view line : held)
    text.append("\n").append(line);
  return text;
}

} // namespace

Restart::Restart(Commands &commands, const Log &log, const std::filesystem::path &build)
    : _state(std::make_unique<State>(commands, log, build)) {
  _state->command = commands.add("quit restart",
                                 "builds vulpen and ends the run for the new vulpen, "
                                 "which replays the edits the views' files do not hold, "
                                 "keeps the current view and counts on from this frame; "
                                 "refused while a node holds work, as quit is, and a "
                                 "build that fails ends nothing",
                                 *this);
}

Restart::~Restart() {
  _state->commands.remove(_state->command);
}

bool Restart::asked() const {
  return _state->asked;
}

bool Restart::ready(const Live *live) {
  State &state = *_state;
  if (!state.asked)
    return false;
  if (!state.building) {
    if (!live || !live->building()) {
      state.builder.start();
      state.building = true;
    }
    return false;
  }
  const std::optional<bool> built = state.builder.done();
  if (!built)
    return false;
  state.asked = state.building = false;
  const std::vector<std::string_view> held = state.commands.holding();
  if (!*built)
    state.log.write(Level::error,
                    Tag::mod,
                    "quit restart: the build failed, so the run goes on\n" +
                        state.builder.said());
  else if (!held.empty())
    state.log.write(Level::error, Tag::run, refusal(held));
  else
    state.log.write(Level::info,
                    Tag::run,
                    std::format("quit restart: built in {:.2f} s; the new vulpen runs on",
                                state.builder.took().count()));
  return *built && held.empty();
}

void Restart::command(Call &call) {
  if (const std::vector<std::string_view> held = _state->commands.holding(); !held.empty())
    throw std::runtime_error(refusal(held));
  call.reply(_state->asked ? "vulpen builds already; the run restarts once it is built"
                           : "vulpen builds; the run restarts once it is built, and goes "
                             "on as it is if the build fails");
  _state->asked = true;
}

} // namespace VP
