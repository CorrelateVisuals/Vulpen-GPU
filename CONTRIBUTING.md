# Contributing

Thank you for trying Vulpen. Three ways in, from the smallest:

1. **Tell us whether it runs on your GPU.** A [GPU report](https://github.com/CorrelateVisuals/Vulpen-GPU/issues/new?template=gpu-report.yml) takes two minutes, and it is how Vulpen learns which hardware it runs on, working or not.
2. **Report a bug** with the [bug form](https://github.com/CorrelateVisuals/Vulpen-GPU/issues/new?template=bug.yml). Type `log save bug.log` in the terminal before you quit and attach the file: a session's log replays to the same graph, so a bug that comes with one is half fixed.
3. **Send a change.** Issues labelled [good first issue](https://github.com/CorrelateVisuals/Vulpen-GPU/labels/good%20first%20issue) are small and stand alone. Before a larger change, say what you plan in an issue, so no one's work is wasted.

## Build and test

The README's [quick start](README.md#quick-start) names what to install. Before you push:

```bash
python3 src/tools/check.py quick   # configure, build, test, and the names MSVC would refuse
```

`python3 src/tools/check.py install-hook` runs it before each `git push`. For a change to the engine (`src/baseclasses/`, `src/runtime/`), also run `check.py full`: every example made again by typing, every command tried in each state, and AddressSanitizer, about half an hour on Linux. The tests run headless on lavapipe, so they need no GPU and no display.

## How designs are decided

[Principles](docs/architecture/principles.md) and [requirements](docs/architecture/requirements.md) decide every design, and on a conflict the lower ID wins. A pull request names the IDs it serves, and says so if it goes against one. The ones a first change meets most:

- **Features are recipes (V05).** The engine offers general ports. A feature is a part or a component in [`src/runtime/recipes/vulpen/library/`](src/runtime/recipes/vulpen/library), never engine code.
- **Every action is a command (V06)**, which declares its usage, help and completion in one place (RV04).
- **Fail early and loud (A02).** A mistake shows at load or build and names its cause.
- **Small enough to read (A00).** One change a pull request, small enough to read before it lands.
- **No warnings (RP01)**, on Linux and on Windows: GCC and Clang with `-Wall -Wextra -Wpedantic -Wshadow`, MSVC with `/W4`, each warning an error.

## Style

- C++20 and GLSL (RC00); C++ as [`.clang-format`](.clang-format) formats it (RC02).
- Types `CamelCase`, functions and variables `snake_case`, members `_snake_case` (RC01).
- Comments say why, not what (C07).
- A commit's subject is `area: what changed`, under 60 characters, as `git log` shows them; the why goes in its body.
- The build rewrites [`docs/statistics/LOC.md`](docs/statistics/LOC.md): commit it with your change.

## License

Vulpen is licensed under the [Mozilla Public License 2.0](LICENSE). A pull request is offered under the same license.
