# Include map

Every engine file in `src/` and what it includes: the one page that shows the structure (A00). [`src/tools/gates/include-map.py`](../../src/tools/gates/include-map.py) fails the build when this page and the code disagree, or when the includes form a cycle (RA00). So a new engine file or include edge lands only once a person has written it here, and the page cannot go stale.

The includes form a flow (A06): `Runtime.cpp` at the top includes the most, `Platform.h` and `Log.h` at the bottom are included the most, and no file in between does both. Each header includes less than its `.cpp` (CPP13).

Paths are from `src/`. The standard library is left out, since every file may use it; other libraries appear as `<name>`.

| File | Includes |
| --- | --- |
| `baseclasses/Engine.cpp` | `baseclasses/Engine.h` `baseclasses/Mechanics.h` `baseclasses/Offscreen.h` `baseclasses/Pipelines.h` `baseclasses/Resources.h` `baseclasses/Swapchain.h` |
| `baseclasses/Engine.h` | `<vulkan/vulkan.h>` `baseclasses/Passes.h` |
| `baseclasses/GpuLayout.glsl` | — |
| `baseclasses/Log.cpp` | `baseclasses/Log.h` `baseclasses/Platform.h` |
| `baseclasses/Log.h` | — |
| `baseclasses/Mechanics.cpp` | `baseclasses/Log.h` `baseclasses/Mechanics.h` `baseclasses/Platform.h` |
| `baseclasses/Mechanics.h` | `<vulkan/vulkan.h>` |
| `baseclasses/Offscreen.cpp` | `baseclasses/Mechanics.h` `baseclasses/Offscreen.h` `baseclasses/Resources.h` |
| `baseclasses/Offscreen.h` | — |
| `baseclasses/Passes.cpp` | `baseclasses/Passes.h` |
| `baseclasses/Passes.h` | `<vulkan/vulkan.h>` |
| `baseclasses/Pipelines.cpp` | `baseclasses/Mechanics.h` `baseclasses/Pipelines.h` |
| `baseclasses/Pipelines.h` | `baseclasses/Resources.h` |
| `baseclasses/Platform.cpp` | `<GLFW/glfw3.h>` `<dlfcn.h>` `<time.h>` `<unistd.h>` `<windows.h>` `baseclasses/Platform.h` |
| `baseclasses/Platform.h` | `<vulkan/vulkan.h>` |
| `baseclasses/Resources.cpp` | `<vk_mem_alloc.h>` `baseclasses/Mechanics.h` `baseclasses/Resources.h` |
| `baseclasses/Resources.h` | `<vulkan/vulkan.h>` |
| `baseclasses/Swapchain.cpp` | `baseclasses/Log.h` `baseclasses/Mechanics.h` `baseclasses/Platform.h` `baseclasses/Swapchain.h` |
| `baseclasses/Swapchain.h` | `<vulkan/vulkan.h>` |
| `main.cpp` | `commit.h` `runtime/Runtime.h` |
| `runtime/Commands.cpp` | `baseclasses/Platform.h` `runtime/Commands.h` |
| `runtime/Commands.h` | `runtime/Operator.h` |
| `runtime/Edits.cpp` | `runtime/Commands.h` `runtime/Edits.h` `runtime/View.h` |
| `runtime/Edits.h` | `runtime/Operator.h` |
| `runtime/Manifest.cpp` | `baseclasses/Platform.h` `runtime/Edits.h` `runtime/Manifest.h` `runtime/View.h` |
| `runtime/Manifest.h` | — |
| `runtime/Operator.h` | `<glm/vec2.hpp>` `<glm/vec3.hpp>` `<glm/vec4.hpp>` `baseclasses/Log.h` |
| `runtime/Ports.cpp` | `baseclasses/Platform.h` `runtime/Commands.h` `runtime/Ports.h` |
| `runtime/Ports.h` | `runtime/Operator.h` |
| `runtime/Recipes.cpp` | `baseclasses/Platform.h` `linked-recipes.h` `runtime/Recipes.h` |
| `runtime/Recipes.h` | `runtime/Operator.h` |
| `runtime/Runtime.cpp` | `baseclasses/Engine.h` `baseclasses/Log.h` `baseclasses/Platform.h` `runtime/Commands.h` `runtime/Edits.h` `runtime/Manifest.h` `runtime/Ports.h` `runtime/Recipes.h` `runtime/Runtime.h` `runtime/Schedule.h` `runtime/View.h` `runtime/Views.h` |
| `runtime/Runtime.h` | — |
| `runtime/Schedule.cpp` | `baseclasses/Passes.h` `baseclasses/Pipelines.h` `runtime/Commands.h` `runtime/Operator.h` `runtime/Recipes.h` `runtime/Schedule.h` `runtime/View.h` |
| `runtime/Schedule.h` | `<vulkan/vulkan.h>` `baseclasses/Log.h` `baseclasses/Resources.h` |
| `runtime/View.h` | — |
| `runtime/Views.cpp` | `runtime/Commands.h` `runtime/Manifest.h` `runtime/Schedule.h` `runtime/View.h` `runtime/Views.h` |
| `runtime/Views.h` | `runtime/Commands.h` |
| `tools/tests/Barriers.cpp` | `baseclasses/Passes.h` |

## Recipe code

A file under a folder named `recipes/` is recipe code, not engine (V05), so it has no row above: a new recipe file builds with no edit to this page. The gate checks each of its includes against one rule instead. Recipe code may include a file in its own folder, and a contract as `contracts/<Name>.glsl`, from the nearest `recipes/` folder above it (V03, RV05). Besides those and the standard library, it may include only what this table lists. Engine code never includes recipe code (RA00).

| Recipe code may include | Why |
| --- | --- |
| `runtime/Operator.h` | a node's behaviour and the general ports |
| `runtime/View.h` | the graph, for a part that reads it |
| `baseclasses/GpuLayout.glsl` | the GPU layout every shader declares (RV02) |
| `<glm/*>` | GLSL's vectors and matrices in C++, name for name |
| `<stb_truetype.h>` | the glyph atlas the `font` part bakes |
