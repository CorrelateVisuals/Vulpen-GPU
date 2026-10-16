# Vulpen

Vulpen is a fractal basis for anything that benefits from high performance computing or real-time visuals. C++ and GLSL are written side by side, without worrying about moving data from one to the other, and Vulpen attaches easily to existing projects for visual generation and parallel computation.

## Run

```bash
./run.sh [--release] [--cli | --ide | --<project> | view.vlp] [arguments]     # Linux
.\run.ps1 [--release] [--cli | --ide | --<project> | view.vlp] [arguments]    # Windows
```

Configures, builds and runs the `debug` preset, or `release` with `--release`; the binary lands in `out/build/<preset>/`. `--cli` runs the cli alone, a prompt at the terminal; `--ide` runs the ide, a window; `--<project>` runs a project alone, of those the cli and the ide make in `projects/`, as the example projects `wave` and `triangle` ([usage](docs/usage.md#12-the-cli)); and with no view named the scripts run the one their line `default=--ide` names.

`--log error|warn|info|debug` sets how much the run says, the build included. The default, `warn`, prints only problems and keeps a build that succeeds silent; `debug` prints the build's output too. [`src/baseclasses/Log.h`](src/baseclasses/Log.h) lists what each level adds and what each `{tag}` marks, and a node's `log` word in the manifest sets the level of the lines about that node.

`./run.sh --wave --log info` runs the example project `wave` headless; `./run.sh --triangle` opens a window and draws a triangle whose corners and color come from C++, and `out/build/debug/baseclasses/examples/triangle/triangle` draws one with the baseclasses alone, without the runtime. In the `debug` preset, save any C++ or GLSL of a view while it runs and the running view swaps in the change; see [docs/plans/live-code.md](docs/plans/live-code.md).

## Use

[docs/usage.md](docs/usage.md) shows how each part works today by example: running a view, the manifest, nodes and their folders, commands, recipes, the CLI, child views and the library.

## Test

```bash
ctest --preset debug                 # after a build; also release, asan, tsan, gpu-validation
src/tools/tests/nightly.sh           # every preset, the soaks and the long fuzz
```

[docs/plans/tests.md](docs/plans/tests.md) says what each test proves and what each preset runs.
