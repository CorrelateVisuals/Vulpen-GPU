#include "runtime/Live.h"

#include "baseclasses/Log.h"
#include "baseclasses/Platform.h"
#include "runtime/Commands.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <iterator>
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
// A view's target, by the name the build tree mirrors it by, as nodes.cmake names it, and
// the engine's, which builds the library's too.
constexpr std::string_view view_target = "vulpen_view_";
constexpr std::string_view engine_target = "vulpen";
constexpr std::string_view library_view = "library"; // the recipes', as nodes.cmake names it

// When the file was last written; the earliest time there is while it is not there.
std::filesystem::file_time_type written(const std::filesystem::path &file) {
  std::error_code gone;
  const auto time = std::filesystem::last_write_time(file, gone);
  return gone ? std::filesystem::file_time_type::min() : time;
}

// A file's lines that hold anything, as paths, without the \r a Windows build ends each
// with: one kept would name no view the build has, and the live build would build none.
std::vector<std::filesystem::path> lines_of(const std::filesystem::path &file) {
  std::vector<std::filesystem::path> lines;
  const std::string text = Files::text(file);
  for (std::size_t at = 0; at < text.size();) {
    const std::size_t end = std::min(text.find('\n', at), text.size());
    const std::size_t stop = end > at && text[end - 1] == '\r' ? end - 1 : end;
    if (stop > at)
      lines.emplace_back(text.substr(at, stop - at));
    at = end + 1;
  }
  return lines;
}

// A build of the tree off the frame thread, which takes the tree's build.lock first, and
// builds its targets one at a time, so one that fails holds back no other; what each
// said goes to one file, in turn.
class Builder {
public:
  // log: the file in the build folder what it says goes to.
  Builder(const std::filesystem::path &build, std::string_view log)
      : _build(build), _log_file(build / log), _lock_file(build / build_lock),
        _views_file(build / VP_VIEWS_FILE) {}

  void start(std::vector<std::string> targets) {
    _running = true;
    _started = std::chrono::steady_clock::now();
    _failure.clear();
    _failed.clear();
    _thread = std::jthread([this, targets = std::move(targets)] {
      // A build that cannot take the lock fails, naming why, rather than ending the run
      // as an exception leaving a thread would.
      try {
        const FileLock lock(_lock_file);
        bool first = true;
        for (const std::string &target : targets)
          if (built(target) && Shell::run(command(target, std::exchange(first, false))) != 0)
            _failed.push_back(target);
      } catch (const std::exception &failure) {
        _failure = failure.what();
        _failed = targets;
      }
      _finished = true;
      _running = false;
    });
  }
  bool running() const {
    return _running;
  }
  // Once, as it ends: whether every target built.
  std::optional<bool> done() {
    if (!_finished.exchange(false))
      return std::nullopt;
    return _failed.empty();
  }
  // The targets that failed, read once done said so.
  const std::vector<std::string> &failed() const {
    return _failed;
  }
  std::string said() const {
    return _failure.empty() ? Files::text(_log_file) : _failure;
  }
  std::chrono::duration<double> took() const {
    return std::chrono::steady_clock::now() - _started;
  }

private:
  // Whether the build has the target: a view's only while the build lists the view, as
  // it read the folders last, which a target built before it may have done again.
  bool built(std::string_view target) const {
    if (!target.starts_with(view_target))
      return true;
    const std::vector<std::filesystem::path> views = lines_of(_views_file);
    return std::ranges::find(views, target.substr(view_target.size())) != views.end();
  }

  std::string command(std::string_view target, bool first) const {
    return std::format(R"("{}" --build "{}" --target {} --parallel {} "{}" 2>&1)",
                       VP_CMAKE_COMMAND,
                       _build.string(),
                       target,
                       first ? ">" : ">>",
                       _log_file.string());
  }

  const std::filesystem::path _build;
  const std::filesystem::path _log_file;
  const std::filesystem::path _lock_file;
  const std::filesystem::path _views_file;
  std::chrono::steady_clock::time_point _started{};
  std::atomic<bool> _running{false};
  std::atomic<bool> _finished{false};
  // The thread's, read once _finished says it is done.
  std::vector<std::string> _failed;
  std::string _failure;
  std::jthread _thread;
};

// When a file under the folder was last written, the newest. A name that starts with . is
// no node's and builds nothing, as the loader and the build skip it, so what a part keeps
// there, as the graph its layout, starts no build.
std::filesystem::file_time_type newest(const std::filesystem::path &folder) {
  // Not file_time_type{}: libstdc++ puts the file clock's epoch in the year 2174.
  auto newest = std::filesystem::file_time_type::min();
  std::error_code gone; // a file an editor is replacing may vanish mid-scan
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

} // namespace

struct Live::Scan {
  // A view watched: built once its folder's newest file was seen twice, as built_from.
  struct View {
    std::string name;
    std::filesystem::path folder;
    std::filesystem::file_time_type built_from;
    std::filesystem::file_time_type pending;
  };

  Scan(const std::vector<Root> &roots, const std::filesystem::path &build)
      : builder(build, build_log),
        engine(lines_of(build / VP_ENGINE_SOURCES_FILE)),
        engine_said(written(build / VP_ENGINE_FILE)) {
    for (const auto &[name, folder] : roots) {
      const auto seen = newest(folder);
      views.push_back({name, folder, seen, seen});
    }
  }

  // Once a scan, as a build reads none of them.
  void notice_engine() {
    for (const std::filesystem::path &file : engine)
      if (const auto time = written(file); time > engine_said) {
        engine_said = time;
        engine_change = file;
      }
  }

  std::vector<View> views;
  Builder builder;
  const std::vector<std::filesystem::path> engine; // as the build lists them
  std::filesystem::file_time_type engine_said; // the newest engine file's, once said
  std::filesystem::path engine_change;
  std::vector<std::string> failed; // the views the last build failed on
  std::chrono::steady_clock::time_point next_scan{};
};

Live::Live(std::vector<Root> roots, const std::filesystem::path &build)
    : _scan(std::make_unique<Scan>(roots, build)) {}

Live::~Live() = default;

Live::Build Live::poll() {
  Scan &scan = *_scan;
  const auto now = std::chrono::steady_clock::now();
  Build build = Build::none;
  // The end of a build is read before the next starts, which clears what it failed on.
  if (scan.builder.done()) {
    // A view closed or renamed while it built is gone, and so is its failure; one that
    // failed while files changed under a view, as a line renames them, may have failed
    // on a file gone mid-build, as the build reads every view's folders, so it builds
    // again once they settle, and its failure is said once it fails on files that stayed.
    const bool moved = std::ranges::any_of(scan.views, [](const Scan::View &view) {
      return newest(view.folder) != view.built_from;
    });
    scan.failed.clear();
    for (const std::string &target : scan.builder.failed()) {
      if (target == engine_target)
        continue;
      const std::string name = target.substr(view_target.size());
      const auto view = std::ranges::find(scan.views, name, &Scan::View::name);
      if (view != scan.views.end() && moved)
        view->built_from = std::filesystem::file_time_type::min();
      else if (view != scan.views.end())
        scan.failed.push_back(name);
    }
    build = scan.failed.empty() ? Build::succeeded : Build::failed;
  }
  if (build == Build::none && now >= scan.next_scan && !scan.builder.running()) {
    scan.next_scan = now + scan_interval;
    scan.notice_engine();
    std::vector<std::string> targets;
    bool fresh = false;
    for (Scan::View &view : scan.views) {
      const auto seen = newest(view.folder);
      if (seen != view.built_from && seen == view.pending) {
        fresh = fresh || view.built_from == std::filesystem::file_time_type::min();
        view.built_from = seen;
        targets.push_back(std::format("{}{}", view_target, view.name));
      }
      view.pending = seen;
    }
    // A view no build made yet has a target only once the build read the folders again,
    // which a build of a target it knows does first, as the library's.
    if (const std::string library = std::format("{}{}", view_target, library_view);
        fresh && std::ranges::find(targets, library) == targets.end())
      targets.insert(targets.begin(), library);
    if (!targets.empty()) {
      scan.builder.start(std::move(targets));
      build = Build::started;
    }
  }
  return build;
}

const std::vector<std::string> &Live::failed() const {
  return _scan->failed;
}

std::vector<std::string> Live::targets() const {
  std::vector<std::string> targets;
  for (const Scan::View &view : _scan->views)
    targets.push_back(std::format("{}{}", view_target, view.name));
  return targets;
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

// A view watched already keeps its times; one new to it builds at the next scans, as its
// files were never seen built.
void Live::watch(const std::vector<Root> &roots) {
  std::vector<Scan::View> views;
  for (const auto &[name, folder] : roots) {
    const auto was = std::ranges::find(_scan->views, folder, &Scan::View::folder);
    views.push_back(was != _scan->views.end()
                        ? std::move(*was)
                        : Scan::View{name,
                                     folder,
                                     std::filesystem::file_time_type::min(),
                                     newest(folder)});
  }
  _scan->views = std::move(views);
}

struct Restart::State {
  State(Commands &port, const Log &said, const std::filesystem::path &build)
      : commands(port), log(said), builder(build, restart_log) {}

  Commands &commands;
  const Log &log;
  Builder builder;
  bool asked = false;
  bool building = false;
};

namespace {

// The views of targets, by name, as a line names them.
std::string views_of(const std::vector<std::string> &targets) {
  std::string names;
  for (const std::string &target : targets)
    names.append(names.empty() ? "" : ", ").append(target.substr(view_target.size()));
  return names;
}

std::string refusal(const std::vector<std::string_view> &held) {
  std::string text = "quit restart would lose what nodes hold, so the run goes on:";
  for (const std::string_view line : held)
    text.append("\n").append(line);
  return text;
}

} // namespace

Restart::Restart(Commands &commands, const Log &log, const std::filesystem::path &build)
    : _state(std::make_unique<State>(commands, log, build)) {
  // Never removed: the runtime keeps this past the port, as the run's end waits for a
  // build it runs.
  commands.add("quit restart",
               "builds vulpen and ends the run for the new vulpen, "
               "which replays the edits the views' files do not hold, "
               "keeps the current view and counts on from this frame; "
               "refused while a node holds work, as quit is, and a "
               "build that fails ends nothing",
               *this);
}

Restart::~Restart() = default;

bool Restart::asked() const {
  return _state->asked;
}

bool Restart::building() const {
  return _state->builder.running();
}

bool Restart::ready(const Live *live) {
  State &state = *_state;
  if (!state.asked)
    return false;
  if (!state.building) {
    if (!live || !live->building()) {
      std::vector<std::string> targets{std::string(engine_target)};
      if (live)
        std::ranges::copy(live->targets(), std::back_inserter(targets));
      state.builder.start(std::move(targets));
      state.building = true;
    }
    return false;
  }
  if (!state.builder.done())
    return false;
  state.asked = state.building = false;
  const std::vector<std::string> &failed = state.builder.failed();
  const bool built = std::ranges::find(failed, engine_target) == failed.end();
  const std::vector<std::string_view> held = state.commands.holding();
  if (!built)
    state.log.write(Level::error,
                    Tag::mod,
                    "quit restart: the build failed, so the run goes on\n" +
                        state.builder.said());
  else if (!held.empty())
    state.log.write(Level::error, Tag::run, refusal(held));
  else
    state.log.write(
        failed.empty() ? Level::info : Level::warn,
        Tag::run,
        std::format("quit restart: built in {:.2f} s; the new vulpen runs on{}",
                    state.builder.took().count(),
                    failed.empty() ? std::string()
                                   : std::format(", though {} failed to build, which its "
                                                 "nodes then name\n{}",
                                                 views_of(failed),
                                                 state.builder.said())));
  return built && held.empty();
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
