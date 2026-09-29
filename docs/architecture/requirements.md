# Requirements

Requirements are strict. A design is first made against the [principles](principles.md), then checked here; if it fails, it goes back to design within the principles and is checked again.

**Each section numbers its own requirements** (RV, RA, RC, RVK, RP, RD); a new one takes the next number in its section, so existing IDs never change.

Requirements must never conflict; if two do, fix this file.

**Only the Vulpen section is project-specific.** Its IDs all start with `RV`. Delete it and the rest holds for any C++, Vulkan and GLSL project; general entries never refer to an `RV` ID.

## Vulpen

- **RV00** Recipes never include each other's code.
- **RV01** The Vulkan floor is raised by descriptor indexing and buffer device address; the [GPU layout](#gpu-layout) depends on both.
- **RV02** Every pipeline uses the [GPU layout](#gpu-layout), with offsets and bindings taken from reflection (RA03).
- **RV03** A manifest states its version; an older one is refused with the command that migrates it.
- **RV04** Every command declares its usage, help and argument completion in one place; a command without them does not register.

### GPU layout

One pipeline layout for every pipeline:

- **set 0, binding 0** — `texture2D[]`: every sampled image
- **set 0, binding 1** — `sampler[]`: slots 0–3 are the static samplers
- **set 0, binding 2** — `image2D[]`: every storage image; the shader declares the array once per format
- **set 1, binding 0** — pass UBO: the pass's parameters and handles, offsets from reflection
- **push constants** — address of the frame block (time, frame, resolution, cursor)
- **no descriptor** — buffers, passed as 64-bit device addresses

A handle is an index into a set 0 array; 0 means unbound. UBOs are std140, buffers std430.

## Architecture

- **RA00** Dependency direction: code never includes from code that builds on it.
- **RA01** OS APIs and OS conditionals (`#ifdef _WIN32`, `__linux__`) live only in the platform files.
- **RA02** `src/external-libraries/` is never edited; a library is only replaced whole by a pinned upstream release.
- **RA03** Shaders are the single source of GPU layouts: C++ gets UBO offsets and descriptor bindings from SPIR-V reflection, never from a hand-kept copy.
- **RA04** Loading, reading and running never change files; only a save writes, and a save writes a temp file and renames it over the old one, so a killed run never leaves half a file.

## Code

- **RC00** Languages: C++20 and GLSL; Python, CMake and shell only for tooling and builds. Vulkan is the only graphics/compute API.
- **RC01** Types `CamelCase`; functions and variables `snake_case`; C++ members `_snake_case`.
- **RC02** C++ matches `.clang-format`.
- **RC03** Ownership: `std::unique_ptr` or by value; no `std::shared_ptr`, no naked `new`/`delete`.
- **RC04** No silent failure: every error is logged or propagated.
- **RC05** No phase, plan or task tags (`B3`, `P11.x`, `T4`) in code comments.
- **RC06** Floating point follows IEEE 754 in every build: no fast-math.
- **RC07** Every binary logs the commit it was built from at startup.

## Vulkan

- **RVK00** Runs without Vulkan validation errors.
- **RVK01** Floor: Vulkan 1.2. A project may raise it only by named features; every other optional feature has a fallback.

## Platforms

- **RP00** Linux first: Linux is the primary platform for development and testing.
- **RP01** Builds without C++ warnings and passes the tests.
- **RP02** Paths in code and data files resolve against a known root (the executable or the file that names them), never the working directory.
- **RP03** Uses the host toolchain and never bundles a compiler.

## Documentation

- **RD00** Documents are written in Markdown.
- **RD01** LOC and performance stats live in `docs/statistics/`.
- **RD02** A topic is documented once; other documents link to it.
