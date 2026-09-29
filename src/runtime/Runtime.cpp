#include "runtime/Runtime.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>

namespace VP {

namespace {

constexpr const char *usage =
    "usage: vulpen <view.vlp> [--frames N] [--fps N] [--log error|warn|info|debug]";
constexpr std::uint32_t default_fps = 60;
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

} // namespace

// Notices saves under the view's folder and runs the build off the frame thread. The
// build, not the runtime, knows how C++ and GLSL compile (C02): nothing compiles here.
class Runtime::Live {
public:
  enum class Build { none, succeeded, failed };

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
    if (now >= _next_scan && !_building) {
      _next_scan = now + scan_interval;
      const auto seen = newest();
      if (seen != _built_from && seen == _pending) {
        _built_from = seen;
        _building = true;
        _build = std::jthread([this] {
          _status = std::system(_command.c_str());
          _finished = true;
          _building = false;
        });
      }
      _pending = seen;
    }
    if (!_finished.exchange(false))
      return Build::none;
    return _status == 0 ? Build::succeeded : Build::failed;
  }

  const std::filesystem::path &log_file() const {
    return _log_file;
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

  std::filesystem::path _folder;
  std::filesystem::path _log_file;
  std::string _command;
  std::filesystem::file_time_type _built_from;
  std::filesystem::file_time_type _pending;
  std::chrono::steady_clock::time_point _next_scan{};
  std::atomic<bool> _building{false};
  std::atomic<bool> _finished{false};
  std::atomic<int> _status{0};
  std::jthread _build;
};

Runtime::Runtime(std::span<char *const> arguments)
    : _options(parse(arguments)), _log(_options.log) {}

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
  const std::filesystem::path build = Files::executable().parent_path();
  _views = build / "views";
  _engine.emplace(_log);
  _recipes.emplace(_views);
  _view = std::make_unique<View>(Manifest::load(_options.manifest));
  _schedule = std::make_unique<Schedule>(*_engine, *_recipes, *_view, _views, _log);
  if (!_schedule->ok())
    return 1;
  if constexpr (VP_LIVE)
    _live = std::make_unique<Live>(_view->file.parent_path(), build);
  loop();
  _engine->wait();
  return _schedule->ok() ? 0 : 1;
}

void Runtime::loop() {
  const auto period =
      _options.fps == 0
          ? std::chrono::nanoseconds::zero()
          : std::chrono::nanoseconds(std::chrono::seconds(1)) / _options.fps;
  auto next = std::chrono::steady_clock::now();
  for (std::uint64_t frame = 0; _options.frames == 0 || frame < _options.frames;
       ++frame) {
    _engine->wait();
    switch (_live ? _live->poll() : Live::Build::none) {
      case Live::Build::succeeded:
        swap();
        break;
      case Live::Build::failed:
        _log.write(Level::error,
                   "live: the build failed, so the running code stays\n" +
                       text_of(_live->log_file()));
        break;
      case Live::Build::none:
        break;
    }
    _schedule->cook(frame);
    _engine->run(_schedule->take_clears(), _schedule->passes());
    next = std::max(next + period, std::chrono::steady_clock::now());
    std::this_thread::sleep_until(next);
  }
}

// Between frames, with the GPU idle: swaps what the build rewrote and keeps the rest.
void Runtime::swap() {
  const std::vector<std::string> rewritten = _recipes->rewritten();
  _schedule->drop_operators(rewritten);
  for (const std::string &recipe : rewritten)
    _recipes->unload(recipe);
  std::unique_ptr<View> view;
  try {
    view = std::make_unique<View>(Manifest::load(_options.manifest));
    Schedule::order(*view);
  } catch (const std::exception &failure) {
    view.reset();
    _log.write(Level::error, std::format("{}; the running graph stays", failure.what()));
  }
  _schedule = std::make_unique<Schedule>(
      *_engine, *_recipes, view ? *view : *_view, _views, _log, _schedule.get());
  if (view)
    _view = std::move(view);
  _log.write(Level::info,
             rewritten.empty()
                 ? std::string("live: swapped")
                 : std::format("live: swapped, new modules: {}", joined(rewritten)));
}

} // namespace VP
