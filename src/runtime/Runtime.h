#pragma once

#include "baseclasses/Engine.h"
#include "baseclasses/Platform.h"
#include "runtime/Manifest.h"
#include "runtime/Recipes.h"
#include "runtime/Schedule.h"
#include "runtime/View.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>

namespace VP {

// The top of the tree: owns every module and runs the loop, so main() only builds one
// and a test can build one without a window.
class Runtime {
public:
  explicit Runtime(std::span<char *const> arguments);
  ~Runtime();
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;

  // The exit code: nonzero when the view ends the run with a node in error.
  int run();

private:
  struct Options {
    std::filesystem::path manifest;
    std::uint64_t frames = 0; // 0 runs until stopped
    std::uint32_t fps = 0;    // 0 runs unpaced
    Level log = Level::warn;
  };
  class Live;

  static Options parse(std::span<char *const> arguments);
  void loop();
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
