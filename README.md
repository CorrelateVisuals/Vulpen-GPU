# Vulpen

Vulpen is a fractal basis for anything that benefits from high performance computing or real-time visuals. C++ and GLSL are written side by side, without worrying about moving data from one to the other, and Vulpen attaches easily to existing projects for visual generation and parallel computation.

## Run

```bash
./run.sh [--release] [arguments]     # LinuxW
.\run.ps1 [--release] [arguments]    # Windows
```

Configures, builds and runs the `debug` preset, or `release` with `--release`; the binary lands in `out/build/<preset>/`.

`./run.sh src/examples/wave/view.vlp --log info` runs the example view headless. In the `debug` preset, save any of its C++ or GLSL while it runs and the running view swaps in the change; see [docs/plans/live-code.md](docs/plans/live-code.md).
