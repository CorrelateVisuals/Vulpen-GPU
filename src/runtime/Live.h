#pragma once

#include "runtime/Operator.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace VP {

class Commands;
class Log;

// Live code: the build of the tree runs off the frame thread,
// so the frames go on while it runs, as a save under a view's folder asks for it, and as
// quit restart does. The build, not the runtime, knows how C++ and GLSL compile (C02):
// nothing compiles here. Every build takes the tree's build.lock first, as run.sh and
// run.ps1 do, so two never write one tree at once.

// Notices saves under the views' folders and builds their modules and shaders, a view at
// a time, so a view that fails to build keeps running as it was and holds back no other.
class Live {
public:
  enum class Build { none, started, succeeded, failed };
  // A view it builds: the name the build tree mirrors it by, the library's for the
  // recipes, as src/runtime/cmake/nodes.cmake names its target, and the folder whose
  // saves build it.
  using Root = std::pair<std::string, std::filesystem::path>;

  // build: the build folder, where vulpen is.
  Live(std::vector<Root> roots, const std::filesystem::path &build);
  ~Live();
  Live(const Live &) = delete;
  Live &operator=(const Live &) = delete;

  // Once a frame. A change must be seen twice, so a file still being written never
  // builds; a view first watched after the start, as a project loaded, builds at once,
  // as no build may have made it yet: run.sh builds only the projects it opens. Failed
  // when any view failed to build.
  Build poll();
  // The views the last build failed on, by name; the others built.
  const std::vector<std::string> &failed() const;
  // The targets of the views it watches, as nodes.cmake names them.
  std::vector<std::string> targets() const;
  // An engine file written since this vulpen was built and since the last call said one,
  // the newest; empty for none. Only a restart runs the engine anew.
  std::filesystem::path engine_changed();
  bool building() const;
  // What the last build said, or why it could not run.
  std::string said() const;
  // Since the last build started.
  std::chrono::duration<double> took() const;
  // A view hosted or no longer hosted brings its folder or takes it along.
  void watch(const std::vector<Root> &roots);

private:
  struct Scan;
  std::unique_ptr<Scan> _scan;
};

// quit restart: builds vulpen, once no live build runs, and the views hosted, whose
// modules a new engine loads only once built against it, then ends the run for the new
// vulpen to run in its place with what the views' files do not hold yet replayed, so a
// change to the engine runs with the session as it was (V08). A view that fails to build
// keeps no one from the new vulpen, which names its nodes' failure, as ./run.sh starts.
class Restart final : public CommandHandler {
public:
  Restart(Commands &commands, const Log &log, const std::filesystem::path &build);
  ~Restart();
  Restart(const Restart &) = delete;
  Restart &operator=(const Restart &) = delete;

  bool asked() const;
  // Whether its build runs, which the run's end waits for.
  bool building() const;
  // Between frames: whether the run ends now, for the new vulpen. A build that fails, or
  // a node holding work by the time it is built, leaves the run as it is, saying why.
  bool ready(const Live *live);

private:
  struct State;

  void command(Call &call) override;

  std::unique_ptr<State> _state;
};

} // namespace VP
