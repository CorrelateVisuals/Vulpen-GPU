# Principles

Principles are must-haves that leave room for interpretation; they may pull against each other. A design is made against the principles first, then checked against the [requirements](requirements.md). Code ported into the codebase is held to the same principles as new code.

**Numbering is priority: on conflict the lower ID wins.** Across sections, the section higher on this page wins.

**Only the Vulpen section is project-specific.** Its IDs all start with `V`. Delete it and the rest holds for any C++, Vulkan and GLSL project; general entries never refer to a `V` ID.

## Vulpen

### Core

- **V00** Vulpen's core language is the nodes and their connections, expressed in the manifest (`.vlp` file).
- **V01** Vulpen is fractal and extends itself from itself at the lowest possible abstraction level.
- **V02** Vulpen uses recipes to allow instant deployment of combinations of premade nodes, which can be used and modified as if they were made by hand.
- **V03** A view is a project, not a window: one manifest plus its own copy of every recipe deployed into it. It loads, runs and moves as a whole, and editing a library recipe later never changes it. A view can host other views, so views are fractal too (V01).
- **V04** The manifest language stays small and closed: a few words, each with a closed set of values. A new word needs a job no existing word can do.

### Architecture

- **V05 Features are recipes.** Vulpen itself only offers general ports (input, commands such as graph edits, file access). When a recipe needs something Vulpen lacks, Vulpen gains the general port, never the feature.
- **V06 Every action is a command.** Vulpen offers one command port that every front end uses: the CLI and the GUIs are recipes on it, scripts and agents send it text, and none has a private way in. Every command has a text form, so anything a GUI does can also be typed or scripted.
- **V07 Headless is first-class.** Everything runs without a window or display: batch compute on HPC nodes and servers, and tests, scripts and agents driving Vulpen. A window is one output next to files and streams, never a precondition.
- **V08 Replayable.** A session is its command log: replaying the log headless rebuilds the same graph and, with C01, the same output. A bug report, a test and an HPC job all become a log file, and undo comes for free.
- **V09** A node's log level applies to it and everything nested under it; the nearest setting wins, and none means the process level. This keeps C09's quiet default usable in a large graph.

### Vulkan

- **V10** Barriers come from the graph: each node declares what it reads and writes, and Vulpen places the barriers. V00 already knows the edges, and hand-placed barriers are the most common Vulkan bug.

## Architecture

- **A00 Human agency.** A person oversees and steers the code: the modules, and which includes which, fit on one page; every change is small enough to read before it lands; tools and agents propose while a human decides, above all on structure (a new module or include edge). Code grown faster than it is read ends up steered by nobody.
- **A01 One owner.** Every resource has exactly one owner; everyone else borrows.
- **A02 Fail early and loud.** A mistake surfaces at load or build, naming its cause: an unknown key, a setting nothing reads, a declared contract nothing fulfils. Nothing parses and is then ignored.
- **A03 Runs for years.** Memory stays bounded and nothing leaks, and a long soak run proves both, so the code can grow into a safety-critical system.
- **A04 The bootstrap stays tiny.** `main()` is at most 30 lines and only wires modules together; all behaviour lives in the modules, where tests and other programs can reach it.
- **A05 Fit the architecture in place.** New or important code is shaped to attach at the seams that already exist and to use the modules, ports and types already there, never to grow a parallel path beside them. When it cannot fit, the architecture changes first, as its own step (A00), and the code then lands on it. Code that fits reads like the rest, and the one-page map stays true.

## Coding

- **C00** Build minimalistic: features are only added if used.
- **C01** Deterministic: the same input gives the same output.
- **C02** A single source of truth for all data structures, constants and paths.
- **C03** Optimize for runtime performance; measure before and after.
- **C04** Minimize LOC (lines of code) for every implementation.
- **C05** Build modular: a module is self-contained, so it can be replaced or moved whole.
- **C06** Reuse existing code before extending existing code to fit more use cases, before writing new code.
- **C07** Comments describe only the why (why here, why this order, why at all), not what happens below. If the syntax explains it, leave them out.
- **C08** If applicable for the scope, follow MVC.
- **C09** Detailed logging based on a log level enumeration: quiet by default.
- **C10** Build for as many physical systems as we can manage, including Raspberry Pi and Steam Deck. Exception: a system that lacks a feature the Vulkan floor requires (RVK01), such as Raspberry Pi without descriptor indexing, is supported only once a fallback tier exists; the design keeps that tier possible.
- **C11** Optimize for long-term programming language support: standard features over extensions.
- **C12** Descriptive names; short math locals (`uv`, `n`, `idx`) are fine.
- **C13** No mutable globals or singletons: state has an owner (A01) and reaches code through parameters. This avoids static-init order problems across modules and lets tests run in parallel.
- **C14** No magic numbers: every constant and tunable has a name and one definition, never a bare literal in code.

### C++

- **CPP01** Modern C++.
- **CPP02** `std::uint_fast*_t` / `std::int_fast*_t` for CPU-only locals when relevant. Never in GPU-visible types: their size varies per platform.
- **CPP03** Templates when efficient and applicable in versatility
- **CPP04** Polymorphism when applicable; inheritance at most two levels deep.
- **CPP05** Functions aim for ~30 lines; past that, split off a function. Exception: a Vulkan create-info and the structs it points to (a pipeline's stage and state structs, a render pass's attachments and subpasses) are filled in one function, so every pointer is still valid at the `vkCreate*` call.
- **CPP06** Const by default; `constexpr` where it moves work to compile time.
- **CPP07** Private first.
- **CPP08** Pass by reference unless small or the reference could dangle.
- **CPP09** RAII: the owner's destructor releases the resource.
- **CPP10** No heap allocation on the hot path.
- **CPP11** Vulkan structs are filled with designated initializers (`.sType = …, .size = …`); fields left out are zero. This keeps CPP05's exception blocks flat and shaped like the spec.
- **CPP12** A file stays small enough to read whole (about 800 lines); a longer one says why at its top.

### GLSL

- **GLSL01** Shared code comes from shared includes, never copy-paste.
- **GLSL02** No undefined or vendor-dependent behavior (derivatives in non-uniform control flow, uninitialized reads, out-of-range indices), so every GPU gives the same result. Lavapipe goldens hide undefined behavior that other vendors expose.

### Vulkan

- **VK01** Render passes, not dynamic rendering: they work on Vulkan 1.2 and suit tile-based GPUs.
- **VK02** No stalls in the frame loop: the CPU waits only on the frame's fence.
- **VK03** Data stays on the GPU whenever possible: the CPU schedules, and readbacks are explicit and asynchronous. The bus to the GPU is the bottleneck, most of all on HPC.
- **VK04** Vulkan is the native language: where Vulkan already names a thing, use that name (`index_count`, `instance_count`, `VkFormat` names), never a synonym.
