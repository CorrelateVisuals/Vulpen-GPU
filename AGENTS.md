# Agents

What a coding agent needs in this repository on top of what people read. The [README](README.md) says what Vulpen is and how to run it, and [CONTRIBUTING.md](CONTRIBUTING.md) says how to build, test and send a change. This file links to them rather than repeating them.

## Read first

1. [README.md](README.md): the manifest language and the example projects.
2. The [principles](docs/architecture/principles.md) and [requirements](docs/architecture/requirements.md): every design is checked against them, and on a conflict the lower ID wins. A change names the IDs it serves.
3. [`projects/triangles/`](projects/triangles): the same triangle in each project, one idea further each time. Read each `view.vlp` first.

## Build and test with no GPU and no display

- Once the [prerequisites](README.md#prerequisites) are installed, `python3 src/tools/check.py quick` configures, builds and tests in about a minute. On Windows, use `python src\tools\check.py quick` and `.\run.ps1`, with the same flags as `./run.sh`.
- The tests run on lavapipe, Mesa's Vulkan on the CPU (`sudo apt install mesa-vulkan-drivers`), so a container or a CI runner needs no GPU.
- Set `DISPLAY=` and `WAYLAND_DISPLAY=` empty for builds and test runs. Otherwise some tests open windows on the person's screen.
- A change to the engine (`src/baseclasses/`, or `src/runtime/` outside `recipes/`) also needs `check.py full`, which takes about half an hour and most of the machine.

## Drive Vulpen as text

Every action is a command (V06), and every command answers (RV10).

- `./run.sh --cli` runs the command line with no window. Write lines to its input. Each line prints its answer, and a refused line names its cause and the usage it missed. `help` lists the commands. The run ends when its input ends, so keep the input open while a line still needs frames.
- Flags: `--source <file>` runs a file of lines first, `--frames <n>` ends the run after n frames, `--size <w>x<h>` gives a run without a window its frame size, and `--log info` makes the run say more.
- `log save <file>` writes the session as lines. `--source <file>` replays them to the same graph (V08), so a bug report can carry one.
- To get numbers, run `./run.sh --wave --log info`: it fills a buffer on the GPU, and its `probe` node prints it. `probe stats` prints what each node costs per frame on the CPU and the GPU.
- To get pictures, use the [`snapshot`](src/runtime/recipes/vulpen/library/parts/snapshot) part: it saves a PNG to the path in its `file` param, with no window open, and with `frames=N` it records N frames in a row for a video. [`update-readme-images.py`](src/tools/update-readme-images.py) and [`examples-replay.py`](src/tools/examples-replay.py) both drive headless runs by lines, so copy from them.

## Take care

- Keep counts and sizes small on the person's GPU, and try large ones on lavapipe. Vulpen refuses what does not fit the device's memory budget (RVK02), but a buffer the CPU can see comes out of the machine's RAM.
- You propose, and a person decides (A00). Keep each change small enough to read before it lands. Ask before adding a module, an include edge or an engine command.
- How designs are decided, style, commit subjects and the statistics file: see [CONTRIBUTING.md](CONTRIBUTING.md#how-designs-are-decided).
