# Third-party libraries

Everything in `src/external-libraries/` is a pinned upstream release, byte-identical to it and never edited (RA02). To upgrade a library, replace it whole from a newer release and update its row and license file here.

| Library | Pinned at | Files in `src/external-libraries/` | License |
|---|---|---|---|
| [VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | `v3.4.0` | `vk_mem_alloc.h` | MIT, [VulkanMemoryAllocator.txt](VulkanMemoryAllocator.txt) |
| [stb](https://github.com/nothings/stb) | commit `2c980bb` (no tagged releases): stb_image v2.30, stb_image_write v1.16, stb_truetype v1.26 | `stb_image.h` `stb_image_write.h` `stb_truetype.h` | MIT or public domain, [stb.txt](stb.txt) |
| [tinyobjloader](https://github.com/tinyobjloader/tinyobjloader) | `v2.0.0rc13` (newest tag) | `tiny_obj_loader.h` | MIT, [tinyobjloader.txt](tinyobjloader.txt) |
| [GLM](https://github.com/g-truc/glm) | `1.0.3` | `glm/` | MIT or Happy Bunny, [glm.txt](glm.txt) |
| [GLFW](https://github.com/glfw/glfw) | `3.5.1` | `glfw/` | zlib, [glfw.md](glfw.md) |

Two libraries are subsets of their release archive, so the repository stays small. The files that are kept are unchanged.

- **GLM** keeps only the header tree (`glm/glm/` in the archive), the same layout as upstream's former `-light` archive. The 1,015-file HTML reference and the tests are left out.
- **GLFW** keeps `CMakeLists.txt`, `CMake/`, `include/`, `src/`, `deps/` (its Wayland protocol files) and its README, license and contributors. `docs/`, `tests/` and `examples/` are left out, so the build has to set `GLFW_BUILD_DOCS`, `GLFW_BUILD_TESTS` and `GLFW_BUILD_EXAMPLES` to `OFF`.

The Vulkan SDK (headers, loader, `glslangValidator`) and Python 3 come from the machine and are not vendored.

## Fonts

A font lives in the folder of the part that reads it, beside its license, so a drop of the part copies both (V03). The file is the upstream release's, byte for byte.

| Font | Release | File | License |
|---|---|---|---|
| [Roboto Mono](https://github.com/googlefonts/robotomono) | Version 3.001, from Google Fonts | `src/recipes/parts/font/RobotoMono-Regular.ttf` | SIL OFL 1.1, [OFL.txt](../../src/recipes/parts/font/OFL.txt) |
