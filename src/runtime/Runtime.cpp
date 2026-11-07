#include "runtime/Runtime.h"

#include "baseclasses/Engine.h"
#include "baseclasses/Log.h"
#include "baseclasses/Platform.h"
#include "runtime/Commands.h"
#include "runtime/Edits.h"
#include "runtime/Live.h"
#include "runtime/Manifest.h"
#include "runtime/Modules.h"
#include "runtime/Ports.h"
#include "runtime/Schedule.h"
#include "runtime/View.h"
#include "runtime/Views.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
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
    "              [--size WxH] [--log error|warn|info|debug] [--log-file FILE]\n"
    "              [--source FILE] [--restart FILE]";
constexpr std::uint32_t default_fps = 60;
constexpr VkExtent2D window_size{.width = 1280, .height = 720};
// Beside vulpen, what quit restart hands the new vulpen to replay.
constexpr const char *restart_file = "restart.log";
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
// through the log, as a replay rebuilds the graph, not the input that drove it.
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
// the choice for the next launch and says so, rather than seem to switch; on a machine
// with no GPU of the other kind it leaves the choice as it was, and says that.
class Preference final : public CommandHandler {
public:
  Preference(Commands &commands,
             const Log &log,
             const Engine &engine,
             std::filesystem::path file,
             std::string prefer)
      : _log(log), _engine(engine), _file(std::move(file)), _prefer(std::move(prefer)),
        _show(commands.add(
            "gpu", "says which GPU runs, and which kind the next launch takes", *this)),
        _toggle(commands.add("gpu toggle",
                             "makes the next launch prefer the other kind of GPU, "
                             "integrated or discrete, than it would take now, where one "
                             "is; this run stays on its own",
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
    const std::string text = Files::text(file);
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

  // The kind the next launch takes, as the engine would choose it among the GPUs it
  // found, so a choice no GPU here meets says what is taken in its place.
  void command(Call &call) override {
    if (call.is(_toggle))
      toggle();
    const std::string_view next = _engine.takes(_prefer);
    const std::string why =
        _prefer.empty()   ? std::string(", the fastest")
        : _prefer == next ? std::string()
                          : std::format(", as no {} one meets the floor here", _prefer);
    call.reply(std::format(
        "runs on {}; the next launch takes the {} GPU{}", _engine.device(), next, why));
  }

  void toggle() {
    const std::string_view now = _engine.takes(_prefer);
    const std::string_view other = now == discrete ? integrated : discrete;
    if (_engine.takes(other) != other) {
      _log.write(Level::warn,
                 Tag::gpu,
                 std::format("gpu toggle: no {} GPU here meets the Vulkan floor, so the "
                             "next launch takes the {} GPU still",
                             other,
                             now));
      return;
    }
    _prefer = other;
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

  // What runs in this vulpen's place once quit restart built it, and the run ended for
  // it: the new vulpen, as the build wrote it where this one was.
  struct Relaunch {
    std::filesystem::path program;
    std::vector<std::string> arguments;
  };

  int run();
  const std::optional<Relaunch> &relaunch() const;

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
    // Where the log keeps what it prints too; empty for nowhere.
    std::filesystem::path log_file;
    // A file of commands, one a line, that the port runs before the first frame.
    std::filesystem::path source;
    // As source, the session quit restart carries, whose failure ends no run.
    std::filesystem::path restart;
  };

  static Options parse(std::span<char *const> arguments);
  bool start();
  void host();
  void loop();
  void watch();
  void swap(std::span<const std::string> failed);
  void open_window(const std::string &title);
  Relaunch restarted(std::uint64_t ran);
  void prepare();
  void gather();
  bool ok() const;

  const Options _options;
  // Its path as it started: a build that replaces the file leaves the running one
  // nameless.
  const std::filesystem::path _executable = Files::executable();
  const Log _log;
  // Node code's std::cout and std::cerr, into the log, from the first bind to the last.
  Printed _printed{_log};
  std::filesystem::path _mirror; // the build tree's views, where the schedules look
  // A build the run's end meets goes on to its end, as one cut short leaves the build
  // tree half written, so these two go after the window and the GPU, and the process
  // ends once the build does.
  std::unique_ptr<Live> _live;
  std::unique_ptr<Restart> _restart;
  std::optional<Desktop> _desktop; // from the first window on, outliving each
  std::optional<Window> _window;   // outlives the engine, which draws into it
  bool _windowless = false;        // the window could not open: no frame tries again
  std::optional<Engine> _engine;
  Modules _modules; // outlives the schedules, whose operators run its code
  Recipes _recipes; // outlives the views and the file port, which unfold from it
  Ports _ports{_log, _recipes};
  Feed _feed{_ports};
  Commands _commands;
  std::optional<Wiring> _wiring; // what every schedule borrows, once the engine exists
  std::optional<Views> _views;
  std::optional<Edits> _edits; // goes before the views it changes
  std::optional<Preference> _preference; // goes before the engine it names
  std::optional<Relaunch> _relaunch;
  // Every view's, in the order a frame runs them; the passes are gathered only when a
  // schedule changed, so a frame allocates nothing for them (CPP10).
  std::vector<Pass> _passes;
  std::vector<VkBuffer> _clears;
  std::vector<Copy> _copies;
};

} // namespace

Runtime::Runtime(std::span<char *const> arguments, std::string_view build)
    : _options(parse(arguments)), _log(_options.log, build, _options.log_file.string()),
      _commands(_log, _ports, _ports) {
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
    } else if (argument == "--restart") {
      options.restart = value;
    } else if (argument == "--log-file") {
      options.log_file = value;
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
    // A line that no longer runs, as one an engine change refuses, leaves the views as
    // replayed so far, rather than ending the run the session goes on in.
    if (!_options.restart.empty())
      try {
        _commands.source(_options.restart, true);
        _log.write(Level::info,
                   Tag::run,
                   std::format("restarted: replayed {}", _options.restart.string()));
      } catch (const std::runtime_error &failure) {
        _log.write(Level::error,
                   Tag::run,
                   std::format("restart: {}; the views go on as replayed so far",
                               failure.what()));
      }
    loop();
  } catch (const std::exception &failure) {
    _log.write(Level::error, Tag::run, failure.what());
    return 1;
  }
  // The last frame's passes may still read the buffers a rebuild would destroy.
  _engine->wait();
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
  const std::filesystem::path build = _executable.parent_path();
  _mirror = build / "views";
  View view = Views::read(_log, _options.manifest);
  const std::filesystem::path folder = Manifest::root(view);
  // Only a view that draws into the window opens one; every other view runs headless
  // (V07), its draws into images at --size.
  const bool shows = draws_into_window(Manifest::flatten(view, _recipes));
  _log.write(Level::info,
             Tag::run,
             std::format("view {} from {}: {}",
                         view.name,
                         view.file.string(),
                         shows ? "a node draws into the window, so it opens one"
                               : "no node draws into the window, so it runs headless"));
  if (shows)
    open_window(title(view));
  _ports.attach(_window ? &*_window : nullptr);
  // The kind of GPU a person chose for the launches of this view.
  const std::filesystem::path choice = view.file.parent_path() / ".vulpen" / "gpu.ini";
  std::string prefer = Preference::read(choice);
  _engine.emplace(_log, _window ? &*_window : nullptr, prefer);
  _preference.emplace(_commands, _log, *_engine, choice, std::move(prefer));
  _restart = std::make_unique<Restart>(_commands, _log, build);
  _wiring.emplace(Wiring{.engine = *_engine,
                         .pipelines = _engine->pipelines(),
                         .resources = _engine->resources(),
                         .render_pass = _engine->render_pass(),
                         .modules = _modules,
                         .log = _log,
                         .views = _mirror,
                         .commands = _commands,
                         .ports = _ports});
  _views.emplace(*_wiring, std::move(view), _commands, _recipes);
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
    if (!_window && _desktop)
      _desktop->poll();
    _engine->wait();
    // While the schedules whose passes the frame before ran still run: a swap or a
    // rebuild below may replace them.
    for (Schedule *const schedule : _views->schedules())
      schedule->time(_engine->times());
    if (_live && !_restart->asked())
      watch();
    // Before this frame cooks, so the new vulpen counts on from it.
    if (_restart->ready(_live.get()))
      try {
        _relaunch = restarted(frames);
        break;
      } catch (const std::exception &failure) {
        _log.write(Level::error,
                   Tag::run,
                   std::format("quit restart: {}; the run goes on", failure.what()));
      }
    if (_ports.relisted())
      _views->relist();
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
  // A run that ended with its terminal still held is never silent.
  if ((_live && _live->building()) || _restart->building())
    _log.write(Level::warn,
               Tag::mod,
               "a build runs: the window and the GPU go now, and vulpen ends once the "
               "build does");
}

void Runtime::watch() {
  switch (_live->poll()) {
    case Live::Build::started:
      _log.write(
          Level::debug, Tag::mod, "a file changed; building the modules and shaders");
      break;
    case Live::Build::failed:
      _log.write(Level::error,
                 Tag::mod,
                 std::format("live: the build of {} failed, so its running code stays\n{}",
                             joined(_live->failed()),
                             _live->said()));
      [[fallthrough]];
    case Live::Build::succeeded:
      _modules.builds_ended(_live->built(), _live->failed());
      // Each module would refuse to load, and every node with C++ would drop out.
      if (!Modules::builds_loadable()) {
        _log.write(Level::warn,
                   Tag::mod,
                   "live: the engine's headers changed, so the modules built now cannot "
                   "load into this vulpen, and none swaps in; quit restart builds the "
                   "engine and runs it");
        break;
      }
      swap(_live->failed());
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

// Between frames, with the GPU idle: swaps what the build rewrote and keeps the rest; a
// view that failed to build keeps its modules, as its nodes share contracts (V03), which
// some built and others not would read otherwise.
void Runtime::swap(std::span<const std::string> failed) {
  std::vector<std::string> rewritten = _modules.rewritten();
  std::erase_if(rewritten, [&](const std::string &module) {
    return std::ranges::any_of(failed, [&](const std::string &view) {
      return module.starts_with(view) && module.size() > view.size() &&
             module[view.size()] == '/';
    });
  });
  prepare();
  for (Schedule *const schedule : _views->schedules())
    schedule->drop_operators(rewritten);
  // A module another build rewrote, as run.sh's, leaves its nodes without their C++
  // until its view rebuilds too.
  std::vector<std::string> views = _live->built();
  for (const std::string &module : rewritten) {
    _modules.unload(module);
    views.push_back(module.substr(0, module.find('/')));
  }
  _views->reload(views);
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
// draw into, and closes once no node of any view draws into it, which the log says at
// the level a run prints by default, as a window gone with the run going on is never
// silent.
void Runtime::prepare() {
  if (_views->shows() != _window.has_value() && !_windowless) {
    _log.write(_window ? Level::warn : Level::info,
               Tag::run,
               _window ? "no node draws into the window, so it closes"
                       : "a node draws into the window, so it opens");
    if (_window) {
      _engine->close();
      _ports.attach(nullptr);
      _window.reset();
    } else {
      open_window(title(_views->view()));
      _ports.attach(_window ? &*_window : nullptr);
    }
    _wiring->render_pass = _engine->render_pass();
  }
  // Before the ports go on to this frame, which forgets what the frame before read.
  const bool rebuilt = _views->rebuild();
  const bool pulled = _views->pull(
      [&](std::string_view view, std::string_view node, std::string_view port) {
        return _ports.was_unread(view, node, port);
      },
      rebuilt);
  if (rebuilt || pulled)
    gather();
}

// A window that cannot open, as with no display, ends no run (V07): the draws into it go
// nowhere, which the log says once, as no later frame tries again. At the start the
// engine is not made yet, and takes the window as it is made.
void Runtime::open_window(const std::string &title) {
  try {
    if (!_desktop)
      _desktop.emplace();
    _window.emplace(*_desktop, title, window_size);
    if (_engine)
      _engine->open(*_window);
  } catch (const std::runtime_error &failure) {
    _window.reset();
    _windowless = true;
    _log.write(Level::error,
               Tag::run,
               std::format("the window cannot open, so the draws into it go nowhere and "
                           "the run goes on: {}",
                           failure.what()));
  }
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

// The view vulpen started with alone, as the replay hosts the views the arguments named;
// the replay in place of a script; the frames on from the next; the other options as
// they were.
Runtime::Relaunch Runtime::restarted(std::uint64_t ran) {
  const std::filesystem::path replay = _executable.parent_path() / restart_file;
  Files::save(replay,
              _views->unsaved(replay.parent_path()) +
                  std::format("view switch {}\n", _commands.current()));
  std::vector<std::string> arguments{_views->view().file.string(),
                                     "--restart",
                                     replay.string(),
                                     "--first-frame",
                                     std::to_string(_options.first_frame + ran),
                                     "--fps",
                                     std::to_string(_options.fps),
                                     "--log",
                                     std::string(level_names[static_cast<std::size_t>(
                                         _options.log)])};
  if (_options.frames != 0)
    arguments.insert(arguments.end(),
                     {"--frames", std::to_string(_options.frames - ran)});
  if (_options.size.width != 0)
    arguments.insert(
        arguments.end(),
        {"--size", std::format("{}x{}", _options.size.width, _options.size.height)});
  if (!_options.log_file.empty())
    arguments.insert(arguments.end(), {"--log-file", _options.log_file.string()});
  return {_executable, std::move(arguments)};
}

const std::optional<Runtime::Relaunch> &Runtime::relaunch() const {
  return _relaunch;
}

// The new vulpen runs only once this one is gone, its window, its GPU and its terminal
// given back, as it takes them all anew.
int run(std::span<char *const> arguments, std::string_view build) {
  std::optional<Runtime::Relaunch> relaunch;
  int code = 0;
  {
    Runtime runtime(arguments, build);
    code = runtime.run();
    relaunch = runtime.relaunch();
  }
  if (relaunch)
    Shell::replace(relaunch->program, relaunch->arguments);
  return code;
}

} // namespace VP
