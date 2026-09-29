# Include map

Every file in `src/` and what it includes: the one page that shows the structure (A00). [`src/tools/gates/include-map.py`](../../src/tools/gates/include-map.py) fails the build when this page and the code disagree, or when the includes form a cycle (RA00). So a new file or include edge lands only once a person has written it here, and the page cannot go stale.

Paths are from `src/`. The standard library is left out, since every file may use it; other libraries appear as `<name>`.

| File | Includes |
| --- | --- |
| `baseclasses/Engine.cpp` | `baseclasses/Engine.h` |
| `baseclasses/Engine.h` | `baseclasses/Mechanics.h` `baseclasses/Pipelines.h` `baseclasses/Resources.h` `baseclasses/Swapchain.h` |
| `baseclasses/GpuLayout.glsl` | — |
| `baseclasses/Log.h` | — |
| `baseclasses/Mechanics.cpp` | `baseclasses/Mechanics.h` |
| `baseclasses/Mechanics.h` | `<vulkan/vulkan.h>` `baseclasses/Log.h` |
| `baseclasses/Pipelines.cpp` | `baseclasses/Pipelines.h` |
| `baseclasses/Pipelines.h` | `baseclasses/Mechanics.h` `baseclasses/Resources.h` |
| `baseclasses/Platform.cpp` | `<dlfcn.h>` `baseclasses/Platform.h` |
| `baseclasses/Platform.h` | `baseclasses/Log.h` |
| `baseclasses/Resources.cpp` | `<vk_mem_alloc.h>` `baseclasses/Resources.h` |
| `baseclasses/Resources.h` | `baseclasses/Mechanics.h` |
| `baseclasses/Swapchain.h` | `baseclasses/Mechanics.h` `baseclasses/Platform.h` |
| `examples/wave/recipes/probe/Probe.comp` | `baseclasses/GpuLayout.glsl` |
| `examples/wave/recipes/probe/Probe.cpp` | `runtime/Operator.h` |
| `examples/wave/recipes/wave/Wave.comp` | `baseclasses/GpuLayout.glsl` |
| `examples/wave/recipes/wave/Wave.cpp` | `runtime/Operator.h` |
| `main.cpp` | `commit.h` `runtime/Runtime.h` |
| `recipes/contracts/Curve.glsl` | — |
| `recipes/contracts/Font.glsl` | — |
| `recipes/contracts/Item.glsl` | — |
| `recipes/contracts/Label.glsl` | — |
| `recipes/contracts/Palette.glsl` | — |
| `recipes/contracts/Rect.glsl` | — |
| `recipes/contracts/Relation.glsl` | — |
| `recipes/parts/command-items/CommandItems.h` | `runtime/Operator.h` |
| `recipes/parts/command-line/CommandLine.h` | `runtime/Operator.h` |
| `recipes/parts/font/Font.h` | `runtime/Operator.h` |
| `recipes/parts/graph/Graph.h` | `runtime/Operator.h` |
| `recipes/parts/hit/Hit.h` | `runtime/Operator.h` |
| `recipes/parts/keys/Keys.h` | `runtime/Operator.h` |
| `recipes/parts/list/List.h` | `runtime/Operator.h` |
| `recipes/parts/modes/Modes.h` | `runtime/Operator.h` |
| `recipes/parts/palette/Palette.h` | `runtime/Operator.h` |
| `recipes/parts/relations/Relations.h` | `runtime/Operator.h` |
| `recipes/parts/split/Split.h` | `runtime/Operator.h` |
| `recipes/parts/text/Text.h` | `runtime/Operator.h` |
| `runtime/Commands.h` | `runtime/View.h` |
| `runtime/Manifest.cpp` | `runtime/Manifest.h` |
| `runtime/Manifest.h` | `runtime/Commands.h` `runtime/View.h` |
| `runtime/Operator.h` | `baseclasses/Log.h` `runtime/Commands.h` |
| `runtime/Recipes.cpp` | `linked-recipes.h` `runtime/Recipes.h` |
| `runtime/Recipes.h` | `baseclasses/Platform.h` `runtime/Commands.h` `runtime/Manifest.h` `runtime/Operator.h` `runtime/View.h` |
| `runtime/Runtime.cpp` | `runtime/Runtime.h` |
| `runtime/Runtime.h` | `baseclasses/Engine.h` `baseclasses/Platform.h` `runtime/Commands.h` `runtime/Manifest.h` `runtime/Recipes.h` `runtime/Schedule.h` `runtime/View.h` |
| `runtime/Schedule.cpp` | `runtime/Schedule.h` |
| `runtime/Schedule.h` | `baseclasses/Engine.h` `runtime/Operator.h` `runtime/Recipes.h` `runtime/View.h` |
| `runtime/View.h` | — |
