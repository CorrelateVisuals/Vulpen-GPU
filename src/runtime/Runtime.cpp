#include "runtime/Runtime.h"

#include "baseclasses/Engine.h"
#include "baseclasses/Log.h"
#include "baseclasses/Platform.h"
#include "runtime/Commands.h"
#include "runtime/Manifest.h"
#include "runtime/Ports.h"
#include "runtime/Recipes.h"
#include "runtime/Schedule.h"
#include "runtime/View.h"
#include "runtime/Views.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace VP {

namespace {

constexpr const char *usage =
    "usage: vulpen <view.vlp> [--frames N] [--first-frame N] [--fps N]\n"
    "              [--log error|warn|info|debug]";
constexpr std::uint32_t default_fps = 60;
constexpr VkExtent2D window_size{.width = 1280, .height = 720};
constexpr auto scan_interval = std::chrono::milliseconds(100);
constexpr const char *build_log = "live-build.log";

template <class T> T number(std::string_view flag, std::string_view text) {
  T value{};
  const auto [end, error] =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size())
    throw std::runtime_error(
        std::format("{} {} is not a whole number\n{}", flag, text, usage));
  return value;
}

std::string text_of(const std::filesystem::path &file) {
  std::ifstream in(file);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string joined(const std::vector<std::string> &names) {
  std::string text;
  for (const std::string &name : names)
    text += (text.empty() ? "" : ", ") + name;
  return text;
}

// Owns every module and runs the loop.
class Runtime {
public:
  Runtime(std::span<char *const> arguments, std::string_view build);
  ~Runtime();
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;

  int run();

private:
  struct Options {
    std::filesystem::path manifest;
    std::uint64_t frames = 0; // 0 runs until stopped
    // Where the frame count starts, so a test reaches years of frames in a moment (A03).
    std::uint64_t first_frame = 0;
    std::uint32_t fps = 0;    // 0 runs unpaced
    Level log = Level::warn;
  };
  class Live;

  static Options parse(std::span<char *const> arguments);
  bool start();
  void loop();
  void watch();
  void swap();

  const Options _options;
  const Log _log;
  std::filesystem::path _views;
  std::optional<Window> _window; // outlives the engine, which draws into it
  std::optional<Engine> _engine;
  Recipes _recipes; // outlives the schedule, whose operators run its modules' code
  Commands _commands;
  Ports _ports;
  std::optional<Wiring> _wiring; // what every schedule borrows, once the engine exists
  std::unique_ptr<View> _view;
  std::unique_ptr<Schedule> _schedule;
  std::unique_ptr<Live> _live;
};

} // namespace

// Notices saves under the view's folder and runs the build off the frame thread. The
// build, not the runtime, knows how C++ and GLSL compile (C02): nothing compiles here.
class Runtime::Live {
public:
  enum class Build { none, started, succeeded, failed };

  Live(std::filesystem::path folder, const std::filesystem::path &build)
      : _folder(std::move(folder)), _log_file(build / build_log),
        _command(std::format(
            R"("{}" --build "{}" --target vulpen_recipes --parallel > "{}" 2>&1)",
            VP_CMAKE_COMMAND,
            build.string(),
            _log_file.string())),
        _built_from(newest()), _pending(_built_from) {}

  // Once a frame. A change must be seen twice, so a file still being written never
  // builds.
  Build poll() {
    const auto now = std::chrono::steady_clock::now();
    Build build = Build::none;
    if (now >= _next_scan && !_building) {
      _next_scan = now + scan_interval;
      const auto seen = newest();
      if (seen != _built_from && seen == _pending) {
        _built_from = seen;
        _building = true;
        _started = now;
        _build = std::jthread([this] {
          _status = Shell::run(_command);
          _finished = true;
          _building = false;
        });
        build = Build::started;
      }
      _pending = seen;
    }
    if (_finished.exchange(false))
      build = _status == 0 ? Build::succeeded : Build::failed;
    return build;
  }

  const std::filesystem::path &log_file() const {
    return _log_file;
  }
  // Since the last build started.
  std::chrono::duration<double> took() const {
    return std::chrono::steady_clock::now() - _started;
  }

private:
  std::filesystem::file_time_type newest() const {
    // Not file_time_type{}: libstdc++ puts the file clock's epoch in the year 2174.
    auto newest = std::filesystem::file_time_type::min();
    std::error_code gone; // a file an editor is replacing may vanish mid-scan
    for (const auto &entry : std::filesystem::recursive_directory_iterator(_folder, gone))
      newest = std::max(newest, entry.last_write_time(gone));
    return newest;
  }

  const std::filesystem::path _folder;
  const std::filesystem::path _log_file;
  const std::string _command;
  std::filesystem::file_time_type _built_from;
  std::filesystem::file_time_type _pending;
  std::chrono::steady_clock::time_point _next_scan{};
  std::chrono::steady_clock::time_point _started{};
  std::atomic<bool> _building{false};
  std::atomic<bool> _finished{false};
  std::atomic<int> _status{0};
  std::jthread _build;
};

Runtime::Runtime(std::span<char *const> arguments, std::string_view build)
    : _options(parse(arguments)), _log(_options.log, build) {}

Runtime::~Runtime() {
  if (_engine)
    _engine->wait();
}

Runtime::Options Runtime::parse(std::span<char *const> arguments) {
  Options options{.fps = default_fps};
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view argument = arguments[index];
    if (!argument.starts_with("--")) {
      if (!options.manifest.empty())
        throw std::runtime_error(
            std::format("one view at a time, not {}\n{}", argument, usage));
      options.manifest = argument;
      continue;
    }
    if (index + 1 == arguments.size())
      throw std::runtime_error(std::format("{} needs a value\n{}", argument, usage));
    const std::string_view value = arguments[++index];
    if (argument == "--frames") {
      options.frames = number<std::uint64_t>(argument, value);
    } else if (argument == "--first-frame") {
      options.first_frame = number<std::uint64_t>(argument, value);
    } else if (argument == "--fps") {
      options.fps = number<std::uint32_t>(argument, value);
    } else if (argument == "--log") {
      const std::optional<Level> level = level_named(value);
      if (!level)
        throw std::runtime_error(std::format("--log {} is no level\n{}", value, usage));
      options.log = *level;
    } else {
      throw std::runtime_error(std::format("unknown argument {}\n{}", argument, usage));
    }
  }
  return options;
}

int Runtime::run() {
  if (_options.manifest.empty()) {
    std::puts(usage);
    return 0;
  }
  // A failure that ends the run is a log line like any other error (RC04).
  try {
    if (!start())
      return 1;
    loop();
  } catch (const std::exception &failure) {
    _log.write(Level::error, Tag::run, failure.what());
    return 1;
  }
  return _schedule->ok() ? 0 : 1;
}

// False when a node is in error.
bool Runtime::start() {
  const std::filesystem::path build = Files::executable().parent_path();
  _views = build / "views";
  _view = std::make_unique<View>(Manifest::load(_options.manifest));
  // Only a view that draws opens a window; every other view runs headless (V07).
  const bool draws = Schedule::draws(*_view);
  _log.write(Level::info,
             Tag::run,
             std::format("view {} from {}: {}",
                         _view->name,
                         _view->file.string(),
                         draws ? "a node draws, so it opens a window"
                               : "no node draws, so it runs headless"));
  if (draws)
    _window.emplace(std::format("{} - vulpen", _view->name), window_size);
  _engine.emplace(_log, _window ? &*_window : nullptr);
  _wiring.emplace(Wiring{.pipelines = _engine->pipelines(),
                         .resources = _engine->resources(),
                         .render_pass = _engine->render_pass(),
                         .recipes = _recipes,
                         .log = _log,
                         .views = _views,
                         .commands = _commands,
                         .input = _ports,
                         .files = _ports,
                         .terminal = _ports});
  _schedule = std::make_unique<Schedule>(*_wiring, *_view);
  if (!_schedule->ok())
    return false;
  if constexpr (VP_LIVE) {
    _live = std::make_unique<Live>(_view->file.parent_path(), build);
    _log.write(Level::info,
               Tag::mod,
               std::format("live code: a save under {} swaps in",
                           _view->file.parent_path().string()));
  }
  return true;
}

void Runtime::loop() {
  const auto period =
      _options.fps == 0
          ? std::chrono::nanoseconds::zero()
          : std::chrono::nanoseconds(std::chrono::seconds(1)) / _options.fps;
  const auto started = std::chrono::steady_clock::now();
  auto next = started;
  std::uint64_t frames = 0;
  for (; _options.frames == 0 || frames < _options.frames; ++frames) {
    if (_window && !_window->poll())
      break;
    _engine->wait();
    if (_live)
      watch();
    _schedule->cook(_options.first_frame + frames);
    _engine->run(_schedule->take_clears(), _schedule->passes());
    next = std::max(next + period, std::chrono::steady_clock::now());
    std::this_thread::sleep_until(next);
  }
  const std::chrono::duration<double> ran = std::chrono::steady_clock::now() - started;
  _log.write(Level::info,
             Tag::run,
             std::format("ran {} frames in {:.2f} s", frames, ran.count()));
}

void Runtime::watch() {
  switch (_live->poll()) {
    case Live::Build::started:
      _log.write(Level::debug, Tag::mod, "a file changed; building its recipes");
      break;
    case Live::Build::succeeded:
      swap();
      break;
    case Live::Build::failed:
      _log.write(Level::error,
                 Tag::mod,
                 "live: the build failed, so the running code stays\n" +
                     text_of(_live->log_file()));
      break;
    case Live::Build::none:
      break;
  }
}

// Between frames, with the GPU idle: swaps what the build rewrote and keeps the rest.
void Runtime::swap() {
  const std::vector<std::string> rewritten = _recipes.rewritten();
  _schedule->drop_operators(rewritten);
  for (const std::string &recipe : rewritten)
    _recipes.unload(recipe);
  std::unique_ptr<View> view;
  try {
    view = std::make_unique<View>(Manifest::load(_options.manifest));
    Schedule::order(*view);
  } catch (const std::exception &failure) {
    view.reset();
    _log.write(Level::error,
               Tag::mod,
               std::format("{}; the running graph stays", failure.what()));
  }
  _schedule =
      std::make_unique<Schedule>(*_wiring, view ? *view : *_view, _schedule.get());
  if (view)
    _view = std::move(view);
  _log.write(Level::info,
             Tag::mod,
             std::format("built in {:.2f} s and swapped{}{}",
                         _live->took().count(),
                         rewritten.empty() ? "" : "; new modules: ",
                         joined(rewritten)));
}

int run(std::span<char *const> arguments, std::string_view build) {
  Runtime runtime(arguments, build);
  return runtime.run();
}

} // namespace VP
