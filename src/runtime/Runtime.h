#pragma once

#include "baseclasses/Engine.h"
#include "baseclasses/Platform.h"
#include "runtime/Recipes.h"
#include "runtime/Schedule.h"
#include "runtime/View.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace VP {

// The top of the tree: owns every module and runs the loop, so main() only builds one
// and a test can build one without a window.
class Runtime {
public:
  // build: what this binary is, which the log names first (RC07).
  Runtime(std::span<char *const> arguments, std::string_view build);
  ~Runtime();
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;

  // The exit code: nonzero when the view ends the run with a node in error.
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
  std::unique_ptr<View> _view;
  std::unique_ptr<Schedule> _schedule;
  std::unique_ptr<Live> _live;
};

} // namespace VP
