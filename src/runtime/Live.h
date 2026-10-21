#pragma once

#include "runtime/Operator.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace VP {

class Commands;
class Log;

// Live code: the build of the tree runs off the frame thread,
// so the frames go on while it runs, as a save under a view's folder asks for it, and as
// quit restart does. The build, not the runtime, knows how C++ and GLSL compile (C02):
// nothing compiles here. Every build takes the tree's build.lock first, as run.sh and
// run.ps1 do, so two never write one tree at once.

// Notices saves under the views' folders and builds their modules and shaders.
class Live {
public:
  enum class Build { none, started, succeeded, failed };

  // build: the build folder, where vulpen is.
  Live(std::vector<std::filesystem::path> folders, const std::filesystem::path &build);
  ~Live();
  Live(const Live &) = delete;
  Live &operator=(const Live &) = delete;

  // Once a frame. A change must be seen twice, so a file still being written never
  // builds.
  Build poll();
  // Whether the modules just built can load: they carry the stamp of the engine headers
  // they were built against, and this vulpen loads only its own (live code rule 6).
  bool loads() const;
  // An engine file written since this vulpen was built and since the last call said one,
  // the newest; empty for none. Only a restart runs the engine anew.
  std::filesystem::path engine_changed();
  bool building() const;
  // What the last build said, or why it could not run.
  std::string said() const;
  // Since the last build started.
  std::chrono::duration<double> took() const;
  // A view hosted or no longer hosted brings its folder or takes it along.
  void watch(std::vector<std::filesystem::path> folders);

private:
  struct Scan;
  std::unique_ptr<Scan> _scan;
};

// quit restart: builds vulpen, once no live build runs, then ends the run for the new
// vulpen to run in its place with what the views' files do not hold yet replayed, so a
// change to the engine runs with the session as it was (V08).
class Restart final : public CommandHandler {
public:
  Restart(Commands &commands, const Log &log, const std::filesystem::path &build);
  ~Restart();
  Restart(const Restart &) = delete;
  Restart &operator=(const Restart &) = delete;

  bool asked() const;
  // Between frames: whether the run ends now, for the new vulpen. A build that fails, or
  // a node holding work by the time it is built, leaves the run as it is, saying why.
  bool ready(const Live *live);

private:
  struct State;

  void command(Call &call) override;

  std::unique_ptr<State> _state;
};

} // namespace VP
