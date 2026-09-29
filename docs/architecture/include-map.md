# Include map

Every file in `src/` and what it includes: the one page that shows the structure (A00). [`src/tools/gates/include-map.py`](../../src/tools/gates/include-map.py) fails the build when this page and the code disagree, or when the includes form a cycle (RA00). So a new file or include edge lands only once a person has written it here, and the page cannot go stale.

Paths are from `src/`. The standard library is left out, since every file may use it; other libraries appear as `<name>`.

| File | Includes |
| --- | --- |
| `baseclasses/Engine.h` | `baseclasses/Mechanics.h` `baseclasses/Pipelines.h` `baseclasses/Resources.h` `baseclasses/Swapchain.h` |
| `baseclasses/GpuLayout.glsl` | — |
| `baseclasses/Log.h` | — |
| `baseclasses/Mechanics.h` | `<vulkan/vulkan.h>` `baseclasses/Log.h` |
| `baseclasses/Pipelines.h` | `baseclasses/Mechanics.h` `baseclasses/Resources.h` |
| `baseclasses/Platform.h` | `baseclasses/Log.h` |
| `baseclasses/Resources.h` | `baseclasses/Mechanics.h` |
| `baseclasses/Swapchain.h` | `baseclasses/Mechanics.h` `baseclasses/Platform.h` |
| `main.cpp` | `commit.h` |
| `runtime/Commands.h` | `runtime/View.h` |
| `runtime/Manifest.h` | `runtime/Commands.h` `runtime/View.h` |
| `runtime/Operator.h` | `baseclasses/Log.h` `runtime/Commands.h` |
| `runtime/Recipes.h` | `runtime/Commands.h` `runtime/Manifest.h` `runtime/View.h` |
| `runtime/Runtime.h` | `baseclasses/Engine.h` `baseclasses/Platform.h` `runtime/Commands.h` `runtime/Manifest.h` `runtime/Recipes.h` `runtime/Schedule.h` `runtime/View.h` |
| `runtime/Schedule.h` | `baseclasses/Engine.h` `runtime/Operator.h` `runtime/View.h` |
| `runtime/View.h` | — |
| `runtime/recipes/cli/CommandLine.h` | `runtime/Operator.h` |
