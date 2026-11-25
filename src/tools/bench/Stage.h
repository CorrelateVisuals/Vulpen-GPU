#pragma once

#include "baseclasses/Engine.h"
#include "runtime/Commands.h"
#include "runtime/Edits.h"
#include "runtime/Manifest.h"
#include "runtime/Modules.h"
#include "runtime/Ports.h"
#include "runtime/Schedule.h"
#include "runtime/View.h"
#include "runtime/Views.h"
#include "tools/bench/Bench.h"
#include "tools/bench/Gpu.h"

#include <filesystem>
#include <memory>
#include <optional>

// The bench's run: what Runtime::start wires its views with, on lavapipe and a
// window of Xvfb's (bench/Gpu.h), its nodes' code from the debug build's mirror, as a dev
// build loads it. A Stage is what every view of a check shares, as a run makes it once;
// a Session is one view's run, with commands and ports of its own, as a port keeps the
// views it was given to look in till it ends.

namespace VP::bench {

struct Stage {
  // level: what the log keeps, which a check reads from the commands' log each frame.
  explicit Stage(const Window &window, Level level = Level::error)
      : log(level, "bench"), engine(log, &window, "cpu") {}
  Log log;
  Recipes recipes;
  Modules modules;
  Engine engine;
};

struct Session {
  Session(Stage &stage, const std::filesystem::path &built)
      : log(stage.log), mirror(built), ports(stage.log, stage.recipes, built),
        commands(stage.log, ports, ports),
        wiring{.engine = stage.engine,
               .pipelines = stage.engine.pipelines(),
               .resources = stage.engine.resources(),
               .render_pass = stage.engine.render_pass(),
               .modules = stage.modules,
               .log = stage.log,
               .views = mirror,
               .commands = commands,
               .ports = ports} {
    ports.listen(commands);
  }
  // The view of the manifest, hosted as a run hosts the one it starts with, with the
  // edits Runtime registers beside it; throws as reading or building it does.
  void start(Stage &stage, const std::filesystem::path &manifest) {
    View view = Views::read(stage.log, manifest);
    views.emplace(wiring, std::move(view), commands, stage.recipes);
    commands.look_in(*views);
    edits.emplace(commands, *views);
  }

  const Log &log;
  std::filesystem::path mirror;
  Ports ports;
  Commands commands;
  Wiring wiring;
  std::optional<Views> views;
  std::optional<Edits> edits;
};

// The debug build's mirror of the views, where a dev build's modules and SPIR-V are.
inline std::filesystem::path debug_mirror(const Rows &rows) {
  return rows.repo() / "out" / "build" / "debug" / "views";
}

} // namespace VP::bench
