#include "runtime/Runtime.h"

#include "baseclasses/Engine.h"
#include "baseclasses/Log.h"
#include "baseclasses/Platform.h"
#include "runtime/Commands.h"
#include "runtime/Edits.h"
#include "runtime/Manifest.h"
#include "runtime/Ports.h"
#include "runtime/Recipes.h"
#include "runtime/Schedule.h"
#include "runtime/View.h"
#include "runtime/Views.h"

#include <algorithm>
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
#include <vector>

namespace VP {

namespace {

constexpr const char *usage =
    "usage: vulpen <view.vlp> [--frames N] [--first-frame N] [--fps N]\n"
    "              [--log error|warn|info|debug] [--source FILE]";
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

// A window's title names the view it shows.
std::string title(const View &view) {
  return std::format("{} - vulpen", view.name);
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
    // A file of commands, one a line, that the port runs before the first frame.
    std::filesystem::path source;
  };
  class Live;

  static Options parse(std::span<char *const> arguments);
  bool start();
  void loop();
  void watch();
  void swap();
  void prepare();
  void gather();
  bool ok() const;

  const Options _options;
  const Log _log;
  std::filesystem::path _mirror; // the build tree's views, where the schedules look
  std::optional<Window> _window; // outlives the engine, which draws into it
  std::optional<Engine> _engine;
  Recipes _recipes; // outlives the schedules, whose operators run its modules' code
  Ports _ports;
  Commands _commands;
  std::optional<Wiring> _wiring; // what every schedule borrows, once the engine exists
  std::optional<Views> _views;
  std::optional<Edits> _edits; // goes before the views it changes
  std::unique_ptr<Live> _live;
  // Every view's, in the order a frame runs them; the passes are gathered only when a
  // schedule changed, so a frame allocates nothing for them (CPP10).
  std::vector<Pass> _passes;
  std::vector<VkBuffer> _clears;
};

} // namespace

// Notices saves under the views' folders and runs the build off the frame thread. The
// build, not the runtime, knows how C++ and GLSL compile (C02): nothing compiles here.
class Runtime::Live {
public:
  enum class Build { none, started, succeeded, failed };

  Live(std::vector<std::filesystem::path> folders, const std::filesystem::path &build)
      : _folders(std::move(folders)), _log_file(build / build_log),
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

  // A view hosted or no longer hosted brings its folder or takes it along.
  void watch(std::vector<std::filesystem::path> folders) {
    _folders = std::move(folders);
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
    for (const std::filesystem::path &folder : _folders)
      for (const auto &entry :
           std::filesystem::recursive_directory_iterator(folder, gone))
        newest = std::max(newest, entry.last_write_time(gone));
    return newest;
  }

  std::vector<std::filesystem::path> _folders;
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
    : _options(parse(arguments)), _log(_options.log, build), _commands(_log, _ports) {}

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
    } else if (argument == "--source") {
      options.source = value;
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
    if (!_options.source.empty())
      _commands.source(_options.source);
    loop();
  } catch (const std::exception &failure) {
    _log.write(Level::error, Tag::run, failure.what());
    return 1;
  }
  prepare();
  return ok() ? 0 : 1;
}

// False when a node is in error.
bool Runtime::start() {
  const std::filesystem::path build = Files::executable().parent_path();
  _mirror = build / "views";
  View view = Manifest::load(_options.manifest);
  const std::filesystem::path folder = Manifest::root(view);
  // Only a view that draws opens a window; every other view runs headless (V07).
  const bool draws = Schedule::draws(Manifest::flatten(view));
  _log.write(Level::info,
             Tag::run,
             std::format("view {} from {}: {}",
                         view.name,
                         view.file.string(),
                         draws ? "a node draws, so it opens a window"
                               : "no node draws, so it runs headless"));
  if (draws)
    _window.emplace(title(view), window_size);
  _engine.emplace(_log, _window ? &*_window : nullptr);
  _wiring.emplace(Wiring{.pipelines = _engine->pipelines(),
                         .resources = _engine->resources(),
                         .render_pass = _engine->render_pass(),
                         .recipes = _recipes,
                         .log = _log,
                         .views = _mirror,
                         .commands = _commands,
                         .ports = _ports});
  _views.emplace(*_wiring, std::move(view), _commands);
  _commands.look_in(*_views);
  _edits.emplace(_commands, *_views);
  if (!ok())
    return false;
  if constexpr (VP_LIVE) {
    _live = std::make_unique<Live>(_views->roots(), build);
    _log.write(Level::info,
               Tag::mod,
               std::format("live code: a save under {} swaps in", folder.string()));
  }
  gather();
  return true;
}

void Runtime::loop() {
  const auto period =
      _options.fps == 0
          ? std::chrono::nanoseconds::zero()
          : std::chrono::nanoseconds(std::chrono::seconds(1)) / _options.fps;
  // Seconds count frames at the run's rate, so a replay sees the same time (C01); an
  // unpaced run counts them at the default rate.
  const double rate = _options.fps == 0 ? default_fps : _options.fps;
  const auto started = std::chrono::steady_clock::now();
  auto next = started;
  std::uint64_t frames = 0;
  for (; !_commands.quitting() && (_options.frames == 0 || frames < _options.frames);
       ++frames) {
    if (_window && !_window->poll())
      break;
    _engine->wait();
    if (_live)
      watch();
    prepare();
    _ports.frame();
    const std::uint64_t frame = _options.first_frame + frames;
    _clears.clear();
    for (Schedule *const schedule : _views->schedules()) {
      schedule->cook(frame);
      std::ranges::copy(schedule->take_clears(), std::back_inserter(_clears));
    }
    _engine->run(_clears, _passes, frame, static_cast<double>(frame) / rate);
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
  prepare();
  for (Schedule *const schedule : _views->schedules())
    schedule->drop_operators(rewritten);
  for (const std::string &recipe : rewritten)
    _recipes.unload(recipe);
  _views->reload();
  // Rebuilt now, not at the next frame, so the line below follows what the swap did and
  // any error it met.
  prepare();
  _log.write(Level::info,
             Tag::mod,
             std::format("built in {:.2f} s and swapped{}{}",
                         _live->took().count(),
                         rewritten.empty() ? "" : "; new modules: ",
                         joined(rewritten)));
}

// The schedules the next frame runs. The window follows the views first (V07): it opens
// before the rebuild that brings the first draw, so the draw has a window to draw into,
// and closes once no node of any view draws.
void Runtime::prepare() {
  if (_views->draws() != _window.has_value()) {
    _log.write(Level::info,
               Tag::run,
               _window ? "no node draws, so the window closes"
                       : "a node draws, so the window opens");
    if (_window) {
      _engine->close();
      _window.reset();
    } else {
      _window.emplace(title(_views->view()), window_size);
      _engine->open(*_window);
    }
    _wiring->render_pass = _engine->render_pass();
  }
  if (_views->rebuild())
    gather();
}

void Runtime::gather() {
  _passes.clear();
  for (const Schedule *const schedule : _views->schedules())
    std::ranges::copy(schedule->passes(), std::back_inserter(_passes));
  if (_live)
    _live->watch(_views->roots());
}

// Whether no node of any view is in error.
bool Runtime::ok() const {
  return std::ranges::all_of(_views->schedules(), &Schedule::ok);
}

int run(std::span<char *const> arguments, std::string_view build) {
  Runtime runtime(arguments, build);
  return runtime.run();
}

} // namespace VP
