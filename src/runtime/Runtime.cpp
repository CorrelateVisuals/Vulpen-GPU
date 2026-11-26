// The top of the tree, which owns every module and runs the loop. It runs past 800 lines
// (CPP12): the flags a run starts with, the loop with its rebuilds and swaps, and the
// commands that choose a GPU and how windows show all reach the modules only it holds
// (A01).
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
#include "runtime/Text.h"
#include "runtime/View.h"
#include "runtime/Views.h"

#include <algorithm>
#include <bit>
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

// "And the seventh angel sounded; and there were great voices in heaven, saying, The
// kingdoms of this world are become the kingdoms of our Lord, and of his Christ; and he
// shall reign for ever and ever." Revelation 11:15

constexpr const char *usage =
    "usage: vulpen <view.vlp> [<view.vlp>...] [--frames N] [--first-frame N] [--fps N]\n"
    "              [--size WxH] [--log error|warn|info|debug] [--log-file FILE]\n"
    "              [--source FILE] [--restart FILE]";
constexpr std::uint32_t default_fps = 60;

// The frame's share of the second's samples, counted from the frame index, so each
// frame's samples fall where its seconds do and none is lost to rounding (C01).
std::size_t sound_frames(std::uint64_t frame, std::uint64_t fps) {
  constexpr std::uint64_t rate = SoundPort::sample_rate;
  return static_cast<std::size_t>((frame + 1) * rate / fps - frame * rate / fps);
}
constexpr VkExtent2D window_size{.width = 1280, .height = 720};
// Beside vulpen, what quit restart hands the new vulpen to replay.
constexpr const char *restart_file = "restart.log";
// How soon and how near a press must follow the one before, of the same button, to count
// one more in a row, as a double click's second does: one place for every part (C02).
constexpr auto press_interval = std::chrono::milliseconds(500);
constexpr float press_reach = 6.0f; // pixels, either way

// Width x height, as 1280x720; a refusal names the whole of it.
VkExtent2D size_of(std::string_view flag, std::string_view text) {
  const std::size_t by = text.find('x');
  const std::optional<std::uint32_t> width = number_in<std::uint32_t>(text.substr(0, by));
  const std::optional<std::uint32_t> height =
      by == std::string_view::npos ? std::nullopt
                                   : number_in<std::uint32_t>(text.substr(by + 1));
  if (!width || !height)
    fail("{} {} is no size: a width and a height, as 1280x720\n{}", flag, text, usage);
  return {*width, *height};
}

template <class T> T number(std::string_view flag, std::string_view text) {
  const std::optional<T> value = number_in<T>(text);
  if (!value)
    fail("{} {} is not a whole number\n{}", flag, text, usage);
  return *value;
}

// A window's title names the view it shows.
std::string title(const View &view) {
  return std::format("{} - vulpen", view.name);
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
  void axis(std::string_view name, float x, float y) override {
    _ports.add({.kind = Event::Kind::axis, .name = std::string(name), .at = {x, y}});
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
    const std::string text = Files::text(file);
    for (const std::string_view line : lines_in(text)) {
      const std::size_t equals = line.find('=');
      if (equals == std::string_view::npos || trim(line.substr(0, equals)) != key)
        continue;
      const std::string_view kind = trim(line.substr(equals + 1));
      if (kind != integrated && kind != discrete)
        fail("{}: {} = {} is no kind of GPU: {} or {}",
             file.string(),
             key,
             kind,
             integrated,
             discrete);
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

// How the windows show what is drawn: the samples a pixel of every view's window, the
// window or the image a host shows one in, whether the window waits for the display, and
// whether it fills its monitor, which it does only while it waits: in mailbox mode a
// compositor on another GPU than vulpen's ran out of memory copying the frames and took
// the desktop down (a hybrid laptop, 11-24). Beside the window command's other forms,
// which the input port answers, as these reach the GPU. A new count waits for the next
// frame's prepare, since no frame may record with pipelines made for the render pass it
// replaces.
class Presenting final : public CommandHandler {
public:
  Presenting(Commands &commands, Engine &engine, Ports &ports)
      : _engine(engine), _ports(ports),
        _vsync_on(commands.add("window vsync on",
                               "presents a frame each refresh of the display, in FIFO "
                               "mode",
                               *this)),
        _vsync_off(
            commands.add("window vsync off",
                         "presents as fast as the GPU draws, in mailbox mode where "
                         "the window offers it; not while the window fills its monitor",
                         *this)),
        _fullscreen_on(
            commands.add("window fullscreen on",
                         "makes the window fill its monitor, while vsync is on",
                         *this)),
        _fullscreen_off(commands.add(
            "window fullscreen off", "puts the window back where and as it was", *this)),
        _samples(commands.add("window samples <value>",
                              "draws every window with that many samples a pixel, "
                              "resolved as its draws end, so their edges come out "
                              "smooth: 1, 2, 4 or 8, of those the GPU offers",
                              *this)) {}

  // What the windows take from the next frame.
  VkSampleCountFlagBits samples() const {
    return _wanted;
  }
  // The count kept, when the GPU had no room for the one asked for.
  void keep(VkSampleCountFlagBits samples) {
    _wanted = samples;
  }

private:
  void command(Call &call) override {
    if (call.is(_samples))
      return call.reply(sample(call.arguments().front()));
    if (call.is(_fullscreen_on) || call.is(_fullscreen_off))
      return call.reply(fullscreen(call.is(_fullscreen_on)));
    const bool on = call.is(_vsync_on);
    if (!on && _ports.fills())
      throw std::runtime_error(
          "vsync stays on while the window fills its monitor; window fullscreen off "
          "first");
    const std::string mode = _engine.vsync(on);
    if (mode.empty())
      return call.reply(std::format(
          "set vsync {}; no window is open, so it waits for one", on ? "on" : "off"));
    call.reply(on ? "made the window present a frame each refresh, in FIFO mode"
               : mode == "FIFO" ? "made the window present in FIFO mode still, as it "
                                  "offers no mailbox mode"
                                : "made the window present as fast as the GPU draws, in "
                                  "mailbox mode");
  }

  std::string fullscreen(bool fills) {
    if (fills && !_engine.vsync())
      throw std::runtime_error(
          "the window fills its monitor only while vsync is on, as a compositor on "
          "another GPU may run out of memory copying frames presented faster; window "
          "vsync on first");
    _ports.fullscreen(fills);
    const std::string done =
        fills ? "made the window fill its monitor" : "put the window back as it was";
    return static_cast<const InputPort &>(_ports).windowed()
               ? done
               : done + "; no window is open, so it waits for one";
  }

  std::string sample(std::string_view word) {
    const VkSampleCountFlags counts = _engine.sample_counts();
    const std::optional<std::uint32_t> count = number_in<std::uint32_t>(word);
    if (!count || !std::has_single_bit(*count) || (counts & *count) == 0) {
      std::string offered;
      constexpr auto least = static_cast<std::uint32_t>(VK_SAMPLE_COUNT_1_BIT);
      constexpr auto most = static_cast<std::uint32_t>(VK_SAMPLE_COUNT_64_BIT);
      for (std::uint32_t bit = least; bit <= most; bit <<= 1)
        if ((counts & bit) != 0)
          offered += std::format("{}{}", offered.empty() ? "" : ", ", bit);
      fail("{} is no count of samples this GPU draws a window with: {}", word, offered);
    }
    _wanted = static_cast<VkSampleCountFlagBits>(*count);
    return std::format(
        "set every window to {} sample{} a pixel", *count, *count == 1 ? "" : "s");
  }

  Engine &_engine;
  Ports &_ports; // the window's, whose fullscreen it sets
  VkSampleCountFlagBits _wanted = VK_SAMPLE_COUNT_1_BIT;
  Command _vsync_on;
  Command _vsync_off;
  Command _fullscreen_on;
  Command _fullscreen_off;
  Command _samples;
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
    std::uint32_t fps = 0; // 0 runs unpaced
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
  // The build tree's views, where the schedules look.
  const std::filesystem::path _mirror = _executable.parent_path() / "views";
  // A build the run's end meets goes on to its end, as one cut short leaves the build
  // tree half written, so these two go after the window and the GPU, and the process
  // ends once the build does.
  std::unique_ptr<Live> _live;
  std::unique_ptr<Restart> _restart;
  std::optional<Desktop> _desktop; // from the first window on, outliving each
  std::optional<Window> _window;   // outlives the engine, which draws into it
  bool _windowless = false;        // the window could not open: no frame tries again
  // How much the session had kept when quit refused the close button's last press.
  std::optional<std::size_t> _close_refused_at;
  std::optional<Engine> _engine;
  Modules _modules; // outlives the schedules, whose operators run its code
  Recipes _recipes; // outlives the views and the file port, which unfold from it
  Ports _ports{_log, _recipes, _mirror};
  Feed _feed{_ports};
  Commands _commands;
  std::optional<Wiring> _wiring; // what every schedule borrows, once the engine exists
  std::optional<Views> _views;
  std::optional<Edits> _edits;           // goes before the views it changes
  std::optional<Preference> _preference; // goes before the engine it names
  std::optional<Presenting> _presenting; // and this before the engine it changes
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

// The views' buffers go before the engine, so the last frame must be done with them
// first. A lost device is said again rather than thrown: a throw out of a destructor
// would end the process before the log's last lines.
Runtime::~Runtime() {
  if (_engine)
    try {
      _engine->wait();
    } catch (const std::exception &failure) {
      _log.write(Level::error, Tag::run, failure.what());
    }
}

// A flag given twice is refused, as its first value would be read and dropped (A02); and
// a flag's name is known before its value is taken, so a misspelt one is named as such.
Runtime::Options Runtime::parse(std::span<char *const> arguments) {
  Options options{.fps = default_fps};
  std::vector<std::string_view> given;
  for (std::size_t index = 1; index < arguments.size(); ++index) {
    const std::string_view argument = arguments[index];
    if (!argument.starts_with("--")) {
      if (options.manifest.empty())
        options.manifest = argument;
      else
        options.hosted.emplace_back(argument);
      continue;
    }
    if (std::ranges::find(given, argument) != given.end())
      fail("{} is given twice\n{}", argument, usage);
    given.push_back(argument);
    const auto value = [&] {
      if (index + 1 == arguments.size())
        fail("{} needs a value\n{}", argument, usage);
      return std::string_view(arguments[++index]);
    };
    if (argument == "--frames") {
      options.frames = number<std::uint64_t>(argument, value());
    } else if (argument == "--first-frame") {
      options.first_frame = number<std::uint64_t>(argument, value());
    } else if (argument == "--fps") {
      options.fps = number<std::uint32_t>(argument, value());
    } else if (argument == "--size") {
      options.size = size_of(argument, value());
    } else if (argument == "--source") {
      options.source = value();
    } else if (argument == "--restart") {
      options.restart = value();
    } else if (argument == "--log-file") {
      options.log_file = value();
    } else if (argument == "--log") {
      const std::string_view named = value();
      const std::optional<Level> level = level_named(named);
      if (!level)
        fail("--log {} is no level\n{}", named, usage);
      options.log = *level;
    } else {
      fail("unknown argument {}{}\n{}",
           argument,
           argument.find('=') == std::string_view::npos
               ? ""
               : "; a value follows its flag after a blank, as --frames 2",
           usage);
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
    // The last frame's passes may still read the buffers a rebuild would destroy.
    _engine->wait();
    prepare();
  } catch (const std::exception &failure) {
    _log.write(Level::error, Tag::run, failure.what());
    return 1;
  }
  return ok() ? 0 : 1;
}

// Each named for its folder, as view load names a project, by a line the log keeps (V08).
// Built before the script runs, as the start view is, so the script may use
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
  _presenting.emplace(_commands, *_engine, _ports);
  _restart = std::make_unique<Restart>(_commands, _log, build);
  _wiring.emplace(Wiring{.engine = *_engine,
                         .pipelines = _engine->pipelines(),
                         .resources = _engine->resources(),
                         .render_pass = _engine->render_pass(),
                         .samples = VK_SAMPLE_COUNT_1_BIT,
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
    // The close button asks as quit does, so a node holding work keeps the run (V06);
    // pressed again once quit refused, with nothing saved, closed or changed since, it
    // ends the run anyway, as quit force does, so a window never stays open for good. A
    // press after more work asks again: the session's count of what it kept only grows,
    // as undo leaves it, so the work done since shows however long the history is.
    if (_window && !_window->poll(_feed)) {
      const std::size_t kept = _commands.session().size();
      _commands.send(_close_refused_at == kept ? "quit force" : "quit");
      if (_commands.quitting())
        break;
      _close_refused_at = kept;
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
    const double seconds = static_cast<double>(frame) / rate;
    _ports.start_sound(sound_frames(frame, static_cast<std::uint64_t>(rate)));
    // Read as the swapchain will be made for it, so C++ lays out what shaders map.
    const VkExtent2D resolution = _window ? _window->size() : _options.size;
    _clears.clear();
    _copies.clear();
    for (Schedule *const schedule : _views->schedules()) {
      schedule->cook(frame, seconds, resolution);
      std::ranges::copy(schedule->take_clears(), std::back_inserter(_clears));
      std::ranges::copy(schedule->take_copies(), std::back_inserter(_copies));
    }
    const glm::vec2 cursor = static_cast<const InputPort &>(_ports).pointer();
    _engine->run(
        _clears, _copies, _passes, frame, seconds, {cursor.x, cursor.y}, _options.size);
    _ports.play_sound();
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
  const Live::Build build = _live->poll();
  switch (build) {
    case Live::Build::started:
      _log.write(
          Level::debug, Tag::mod, "a file changed; building the modules and shaders");
      break;
    case Live::Build::failed:
      _log.write(
          Level::error,
          Tag::mod,
          std::format("live: the build of {} failed, so its running code stays\n{}",
                      joined(_live->failed()),
                      _live->errors()));
      _log.write(Level::debug, Tag::mod, "live: the whole build said\n" + _live->said());
      [[fallthrough]];
    case Live::Build::unsettled:
    case Live::Build::succeeded:
      // A view whose build met files still changing keeps its modules, as a failed one
      // does, till it builds again once they settle; not marked failed, its nodes wait
      // for that build quietly.
      _modules.builds_ended(_live->built(),
                            build == Live::Build::failed ? _live->failed()
                                                         : std::vector<std::string>());
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
    _log.write(
        Level::warn,
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
  // New samples for the windows make their render passes again, so every view's draws
  // get pipelines that fit them before this frame records any.
  if (const VkSampleCountFlagBits samples = _presenting->samples();
      samples != _wiring->samples) {
    try {
      _engine->samples(samples);
      _wiring->samples = samples;
    } catch (const std::runtime_error &failure) {
      _log.write(Level::error,
                 Tag::gpu,
                 std::format("window samples {}: {}; every window keeps {}",
                             static_cast<std::uint32_t>(samples),
                             failure.what(),
                             static_cast<std::uint32_t>(_wiring->samples)));
      _engine->samples(_wiring->samples);
      _presenting->keep(_wiring->samples);
    }
    _wiring->render_pass = _engine->render_pass();
    _views->refit();
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

// The start view alone, as the replay hosts the views the arguments named;
// the replay in place of a script; the frames on from the next; the other options as
// they were.
Runtime::Relaunch Runtime::restarted(std::uint64_t ran) {
  const std::filesystem::path replay = _executable.parent_path() / restart_file;
  Files::save(replay,
              _views->unsaved(replay.parent_path()) +
                  std::format("view switch {}\n", _commands.current()));
  std::vector<std::string> arguments{
      _views->view().file.string(),
      "--restart",
      replay.string(),
      "--first-frame",
      std::to_string(_options.first_frame + ran),
      "--fps",
      std::to_string(_options.fps),
      "--log",
      std::string(level_names[static_cast<std::size_t>(_options.log)])};
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
