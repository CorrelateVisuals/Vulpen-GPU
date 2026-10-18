#include "runtime/Runtime.h"

#include "baseclasses/Engine.h"
#include "baseclasses/Log.h"
#include "baseclasses/Platform.h"
#include "runtime/Commands.h"
#include "runtime/Edits.h"
#include "runtime/Manifest.h"
#include "runtime/Modules.h"
#include "runtime/Ports.h"
#include "runtime/Schedule.h"
#include "runtime/View.h"
#include "runtime/Views.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace VP {

namespace {

constexpr const char *usage =
    "usage: vulpen <view.vlp> [<view.vlp>...] [--frames N] [--first-frame N] [--fps N]\n"
    "              [--size WxH] [--log error|warn|info|debug] [--source FILE]";
constexpr std::uint32_t default_fps = 60;
constexpr VkExtent2D window_size{.width = 1280, .height = 720};
constexpr auto scan_interval = std::chrono::milliseconds(100);
constexpr const char *build_log = "live-build.log";
// In the build folder, which every build of the tree takes first, run.sh's and run.ps1's
// too, so two never write one tree at once.
constexpr const char *build_lock = "build.lock";
// How soon and how near a press must follow the one before, of the same button, to count
// one more in a row, as a double click's second does: one place for every part (C02).
constexpr auto press_interval = std::chrono::milliseconds(500);
constexpr float press_reach = 6.0f; // pixels, either way

template <class T> T number(std::string_view flag, std::string_view text);

// Width x height, as 1280x720.
VkExtent2D size_of(std::string_view flag, std::string_view text) {
  const std::size_t by = text.find('x');
  if (by == std::string_view::npos)
    throw std::runtime_error(
        std::format("{} {} is no size: a width and a height, as 1280x720\n{}",
                    flag,
                    text,
                    usage));
  return {number<std::uint32_t>(flag, text.substr(0, by)),
          number<std::uint32_t>(flag, text.substr(by + 1))};
}

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

// A file's lines, without their line breaks; none when it cannot be read.
std::vector<std::filesystem::path> lines_of(const std::filesystem::path &file) {
  std::ifstream in(file);
  std::vector<std::filesystem::path> lines;
  for (std::string line; std::getline(in, line);)
    if (!line.empty())
      lines.emplace_back(line);
  return lines;
}

// When the file was last written; the earliest time there is while it is not there.
std::filesystem::file_time_type written(const std::filesystem::path &file) {
  std::error_code gone;
  const auto time = std::filesystem::last_write_time(file, gone);
  return gone ? std::filesystem::file_time_type::min() : time;
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

// The window hands its input to the port as the input command does (V07); neither goes
// through the log, as the lead chose on 2026-10-06.
class Feed final : public WindowInput {
public:
  explicit Feed(Ports &ports) : _ports(ports) {}

private:
  void key(std::string_view name, bool down) override {
    _ports.add({.kind = Event::Kind::key, .down = down, .name = std::string(name)});
  }
  void text(std::string_view typed) override {
    _ports.add({.kind = Event::Kind::text, .name = std::string(typed)});
  }
  void pointer(float x, float y) override {
    _pointer = {x, y};
    _ports.add({.kind = Event::Kind::pointer, .at = _pointer});
  }
  void button(std::string_view name, bool down) override {
    _ports.add({.kind = Event::Kind::button,
                .down = down,
                .name = std::string(name),
                .presses = down ? pressed(name) : 0});
  }
  void wheel(float x, float y) override {
    _ports.add({.kind = Event::Kind::wheel, .turn = {x, y}});
  }
  void focus(bool gained) override {
    _ports.add({.kind = Event::Kind::focus, .down = gained});
  }

  // The press's place in its row, which the clock alone tells, so it is counted here, as
  // the window hands it on; the input command is given it.
  std::uint32_t pressed(std::string_view button) {
    const auto now = std::chrono::steady_clock::now();
    const glm::vec2 by = _pointer - _press.where;
    const bool again = button == _press.button && now - _press.at <= press_interval &&
                       std::abs(by.x) <= press_reach && std::abs(by.y) <= press_reach;
    _press = {std::string(button), now, _pointer, again ? _press.presses + 1 : 1};
    return _press.presses;
  }

  // The press before, for the next to count from.
  struct Press {
    std::string button;
    std::chrono::steady_clock::time_point at;
    glm::vec2 where{};
    std::uint32_t presses = 0;
  };

  Ports &_ports;
  glm::vec2 _pointer{};
  Press _press;
};

// The kind of GPU the launches of a view take first, as a person chooses for the
// machine, kept in .vulpen/gpu.ini beside the manifest vulpen starts with and read before
// the engine picks its device. Vulkan cannot move a running device, so gpu toggle records
// the choice for the next launch and says so, as the POC's GPU cell did, rather than seem
// to switch.
class Preference final : public CommandHandler {
public:
  Preference(Commands &commands,
             const Log &log,
             const Engine &engine,
             std::filesystem::path file,
             std::string prefer)
      : _log(log), _engine(engine), _file(std::move(file)), _prefer(std::move(prefer)),
        _show(commands.add(
            "gpu", "says which GPU runs, and which kind the next launch prefers", *this)),
        _toggle(commands.add("gpu toggle",
                             "makes the next launch prefer the other kind of GPU, "
                             "integrated or discrete, than it would take now; this run "
                             "stays on its own",
                             *this)) {}

  // The kind a file's prefer line names, empty for none; a kind no GPU is stops the run,
  // naming the file (A02).
  static std::string read(const std::filesystem::path &file) {
    const auto trimmed = [](std::string_view text) {
      const std::size_t first = text.find_first_not_of(" \t\r");
      return first == std::string_view::npos
                 ? std::string_view()
                 : text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
    };
    const std::string text = text_of(file);
    for (std::size_t at = 0; at < text.size();) {
      const std::size_t end = std::min(text.find('\n', at), text.size());
      const std::string_view line = std::string_view(text).substr(at, end - at);
      at = end + 1;
      const std::size_t equals = line.find('=');
      if (equals == std::string_view::npos || trimmed(line.substr(0, equals)) != key)
        continue;
      const std::string_view kind = trimmed(line.substr(equals + 1));
      if (kind != integrated && kind != discrete)
        throw std::runtime_error(std::format(
            "{}: {} = {} is no kind of GPU: {} or {}", file.string(), key, kind,
            integrated, discrete));
      return std::string(kind);
    }
    return {};
  }

private:
  static constexpr std::string_view key = "prefer";
  static constexpr std::string_view integrated = "integrated";
  static constexpr std::string_view discrete = "discrete";

  void command(Call &call) override {
    if (call.is(_toggle)) {
      const std::string_view now = _prefer.empty() ? _engine.kind() : _prefer;
      _prefer = now == discrete ? integrated : discrete;
      Files::save(_file,
                  std::format("# The kind of GPU vulpen takes first here; gpu toggle "
                              "writes it.\n{} = {}\n",
                              key,
                              _prefer));
      _log.write(Level::warn,
                 Tag::gpu,
                 std::format("the next launch prefers the {} GPU; this run stays on {}",
                             _prefer,
                             _engine.device()));
    }
    const std::string next =
        _prefer.empty() ? "takes the fastest" : "prefers the " + _prefer;
    call.reply(std::format("runs on {}; the next launch {} GPU", _engine.device(), next));
  }

  const Log &_log;
  const Engine &_engine;
  const std::filesystem::path _file;
  std::string _prefer; // empty for the fastest
  Command _show;
  Command _toggle;
};

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
    // The views it hosts from the start, beside the projects, as the cli hosts the ide.
    std::vector<std::filesystem::path> hosted;
    std::uint64_t frames = 0; // 0 runs until stopped
    // Where the frame count starts, so a test reaches years of frames in a moment (A03).
    std::uint64_t first_frame = 0;
    std::uint32_t fps = 0;    // 0 runs unpaced
    // While no window is open: the size of the images draws render into, and of the
    // frame block's resolution, so a run renders headless (V07). Zero renders none.
    VkExtent2D size{};
    Level log = Level::warn;
    // A file of commands, one a line, that the port runs before the first frame.
    std::filesystem::path source;
  };
  class Live;

  static Options parse(std::span<char *const> arguments);
  bool start();
  void host();
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
  Modules _modules; // outlives the schedules, whose operators run its code
  Ports _ports{_log};
  Feed _feed{_ports};
  Commands _commands;
  std::optional<Wiring> _wiring; // what every schedule borrows, once the engine exists
  std::optional<Views> _views;
  std::optional<Edits> _edits; // goes before the views it changes
  std::optional<Preference> _preference; // goes before the engine it names
  std::unique_ptr<Live> _live;
  // Every view's, in the order a frame runs them; the passes are gathered only when a
  // schedule changed, so a frame allocates nothing for them (CPP10).
  std::vector<Pass> _passes;
  std::vector<VkBuffer> _clears;
  std::vector<Copy> _copies;
};

} // namespace

// Notices saves under the views' folders and runs the build off the frame thread. The
// build, not the runtime, knows how C++ and GLSL compile (C02): nothing compiles here.
class Runtime::Live {
public:
  enum class Build { none, started, succeeded, failed };

  Live(std::vector<std::filesystem::path> folders, const std::filesystem::path &build)
      : _folders(std::move(folders)), _log_file(build / build_log),
        _lock_file(build / build_lock), _stamp_file(build / VP_MODULE_STAMP_FILE),
        _command(std::format(
            R"("{}" --build "{}" --target vulpen_modules --parallel > "{}" 2>&1)",
            VP_CMAKE_COMMAND,
            build.string(),
            _log_file.string())),
        _engine(lines_of(build / VP_ENGINE_SOURCES_FILE)),
        _engine_said(written(Files::executable())), _built_from(newest()),
        _pending(_built_from) {}

  // Once a frame. A change must be seen twice, so a file still being written never
  // builds.
  Build poll() {
    const auto now = std::chrono::steady_clock::now();
    Build build = Build::none;
    if (now >= _next_scan && !_building) {
      _next_scan = now + scan_interval;
      notice_engine();
      const auto seen = newest();
      if (seen != _built_from && seen == _pending) {
        _built_from = seen;
        _building = true;
        _started = now;
        _failure.clear();
        _build = std::jthread([this] {
          // A build that cannot take the lock fails, naming why, rather than ending the
          // run as an exception leaving a thread would.
          try {
            const FileLock lock(_lock_file);
            _status = Shell::run(_command);
          } catch (const std::exception &failure) {
            _failure = failure.what();
            _status = -1;
          }
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

  // Whether the modules just built can load: they carry the stamp of the engine headers
  // they were built against, and this vulpen loads only its own (live code rule 6).
  bool loads() const {
    const std::string stamp = text_of(_stamp_file);
    return stamp.substr(0, stamp.find_last_not_of(" \r\n") + 1) == VP_MODULE_STAMP;
  }
  // An engine file written since this vulpen was built and since the last call said one,
  // the newest; empty for none. Only a restart runs the engine anew.
  std::filesystem::path engine_changed() {
    return std::exchange(_engine_change, {});
  }
  // What the last build said, or why it could not run.
  std::string said() const {
    return _failure.empty() ? text_of(_log_file) : _failure;
  }

  // A view hosted or no longer hosted brings its folder or takes it along.
  void watch(std::vector<std::filesystem::path> folders) {
    _folders = std::move(folders);
  }
  // Since the last build started.
  std::chrono::duration<double> took() const {
    return std::chrono::steady_clock::now() - _started;
  }

private:
  // A name that starts with . is no node's and builds nothing, as the loader and the
  // build skip it, so what a part keeps there, as the graph its layout, starts no build.
  std::filesystem::file_time_type newest() const {
    // Not file_time_type{}: libstdc++ puts the file clock's epoch in the year 2174.
    auto newest = std::filesystem::file_time_type::min();
    std::error_code gone; // a file an editor is replacing may vanish mid-scan
    for (const std::filesystem::path &folder : _folders)
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
    for (const std::filesystem::path &file : _engine)
      if (const auto time = written(file); time > _engine_said) {
        _engine_said = time;
        _engine_change = file;
      }
  }

  std::vector<std::filesystem::path> _folders;
  const std::filesystem::path _log_file;
  const std::filesystem::path _lock_file;
  const std::filesystem::path _stamp_file;
  const std::string _command;
  const std::vector<std::filesystem::path> _engine; // as the build lists them
  std::filesystem::file_time_type _engine_said; // the newest engine file's, once said
  std::filesystem::path _engine_change;
  std::filesystem::file_time_type _built_from;
  std::filesystem::file_time_type _pending;
  std::chrono::steady_clock::time_point _next_scan{};
  std::chrono::steady_clock::time_point _started{};
  std::atomic<bool> _building{false};
  std::atomic<bool> _finished{false};
  std::atomic<int> _status{0};
  std::string _failure; // the build thread's, read once _finished says it is done
  std::jthread _build;
};

Runtime::Runtime(std::span<char *const> arguments, std::string_view build)
    : _options(parse(arguments)), _log(_options.log, build), _commands(_log, _ports, _ports) {
  _ports.listen(_commands);
}

Runtime::~Runtime() {
  if (_engine)
    _engine->wait();
}

Runtime::Options Runtime::parse(std::span<char *const> arguments) {
  Options options{.fps = default_fps};
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view argument = arguments[index];
    if (!argument.starts_with("--")) {
      if (options.manifest.empty())
        options.manifest = argument;
      else
        options.hosted.emplace_back(argument);
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
    } else if (argument == "--size") {
      options.size = size_of(argument, value);
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
    host();
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

// Each named for its folder, as view load names a project, by a line the log keeps (V08).
// Built before the script runs, as the view vulpen started with is, so the script may use
// their commands, and the window opens as theirs draw; the views a manifest hosts build
// with the first frame, after the script, as before.
void Runtime::host() {
  if (_options.hosted.empty())
    return;
  for (const std::filesystem::path &file : _options.hosted) {
    const std::string name =
        std::filesystem::absolute(file).parent_path().filename().string();
    _commands.run(std::format("view add {} {}", name, file.string()));
  }
  prepare();
}

// False when a node is in error.
bool Runtime::start() {
  const std::filesystem::path build = Files::executable().parent_path();
  _mirror = build / "views";
  View view = Views::read(_log, _options.manifest);
  const std::filesystem::path folder = Manifest::root(view);
  // Only a view that draws into the window opens one; every other view runs headless
  // (V07), its draws into images at --size.
  const bool shows = Schedule::shows(Manifest::flatten(view));
  _log.write(Level::info,
             Tag::run,
             std::format("view {} from {}: {}",
                         view.name,
                         view.file.string(),
                         shows ? "a node draws into the window, so it opens one"
                               : "no node draws into the window, so it runs headless"));
  if (shows)
    _window.emplace(title(view), window_size);
  _ports.attach(_window ? &*_window : nullptr);
  // The kind of GPU a person chose for the launches of this view.
  const std::filesystem::path choice = view.file.parent_path() / ".vulpen" / "gpu.ini";
  std::string prefer = Preference::read(choice);
  _engine.emplace(_log, _window ? &*_window : nullptr, prefer);
  _preference.emplace(_commands, _log, *_engine, choice, std::move(prefer));
  _wiring.emplace(Wiring{.engine = *_engine,
                         .pipelines = _engine->pipelines(),
                         .resources = _engine->resources(),
                         .render_pass = _engine->render_pass(),
                         .modules = _modules,
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
    // The close button asks as quit does, so a node holding work keeps the run (V06).
    if (_window && !_window->poll(_feed)) {
      _commands.send("quit");
      if (_commands.quitting())
        break;
    }
    _engine->wait();
    // While the schedules whose passes the frame before ran still run: a swap or a
    // rebuild below may replace them.
    for (Schedule *const schedule : _views->schedules())
      schedule->time(_engine->times());
    if (_live)
      watch();
    prepare();
    _ports.frame();
    _commands.frame();
    const std::uint64_t frame = _options.first_frame + frames;
    // Read as the swapchain will be made for it, so C++ lays out what shaders map.
    const VkExtent2D resolution = _window ? _window->size() : _options.size;
    _clears.clear();
    _copies.clear();
    for (Schedule *const schedule : _views->schedules()) {
      schedule->cook(frame, resolution);
      std::ranges::copy(schedule->take_clears(), std::back_inserter(_clears));
      std::ranges::copy(schedule->take_copies(), std::back_inserter(_copies));
    }
    const glm::vec2 cursor = static_cast<const InputPort &>(_ports).pointer();
    _engine->run(_clears,
                 _copies,
                 _passes,
                 frame,
                 static_cast<double>(frame) / rate,
                 {cursor.x, cursor.y},
                 _options.size);
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
      _log.write(
          Level::debug, Tag::mod, "a file changed; building the modules and shaders");
      break;
    case Live::Build::succeeded:
      // Each module would refuse to load, and every node with C++ would drop out.
      if (!_live->loads()) {
        _log.write(Level::warn,
                   Tag::mod,
                   "live: the engine's headers changed, so the modules built now cannot "
                   "load into this vulpen, and none swaps in; quit restart builds the "
                   "engine and runs it");
        break;
      }
      swap();
      break;
    case Live::Build::failed:
      _log.write(Level::error,
                 Tag::mod,
                 "live: the build failed, so the running code stays\n" + _live->said());
      break;
    case Live::Build::none:
      break;
  }
  if (const std::filesystem::path changed = _live->engine_changed(); !changed.empty())
    _log.write(Level::warn,
               Tag::mod,
               std::format("live: {} changed since this vulpen was built, and only a "
                           "restart runs the engine anew: quit restart builds it and runs "
                           "it",
                           changed.string()));
}

// Between frames, with the GPU idle: swaps what the build rewrote and keeps the rest.
void Runtime::swap() {
  const std::vector<std::string> rewritten = _modules.rewritten();
  prepare();
  for (Schedule *const schedule : _views->schedules())
    schedule->drop_operators(rewritten);
  for (const std::string &module : rewritten)
    _modules.unload(module);
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
// before the rebuild that brings the first draw into it, so the draw has a window to
// draw into, and closes once no node of any view draws into it.
void Runtime::prepare() {
  if (_views->shows() != _window.has_value()) {
    _log.write(Level::info,
               Tag::run,
               _window ? "no node draws into the window, so it closes"
                       : "a node draws into the window, so it opens");
    if (_window) {
      _engine->close();
      _ports.attach(nullptr);
      _window.reset();
    } else {
      _window.emplace(title(_views->view()), window_size);
      _engine->open(*_window);
      _ports.attach(&*_window);
    }
    _wiring->render_pass = _engine->render_pass();
  }
  if (_views->rebuild())
    gather();
}

void Runtime::gather() {
  _passes.clear();
  _views->gather(_passes);
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
