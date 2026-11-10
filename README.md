# WorldEngine

English | [Simplified Chinese](README.zh-CN.md)

WorldEngine is a native Windows C++17/CMake game engine built with a modular architecture:
the core engine compiles to the shared library **`WorldRuntime.dll`** with an explicit `WLD_API` export boundary,
gameplay logic compiles to an isolated **`Game.dll`**, and the editor and runtime are host executables.
Third-party dependencies and CLI toolchains are locked in the repository (`vendor/`, `third_party/`, and submodules).

## Modules & Binaries

| Module / Directory | Artifact | Responsibility |
| --- | --- | --- |
| `Engine/` | `WorldRuntime.dll` (CMake target `World`) | Engine core: Application lifecycle, memory allocators, jobs, VFS, RHI (OpenGL 4.6 / Vulkan), Slang shader pipeline, ECS & schema reflection, WUI framework, Level 2.0 & Data Layers, Prefab 2.0, Luau VM, and 2D/3D physics |
| `Game/` | `Game.dll` | Gameplay logic. An independent DLL that plugs into the engine through **explicit ABI registration interfaces**; never duplicates engine singletons or global state |
| `Editor/` | `Editor.exe` | Visual editor host: Schema-driven WUI panels, 3D viewport with multi-mode camera & adaptive ruler grid, Level & Prefab authoring, Material/Model inspectors, and Play in Editor |
| `Runtime/` | `Runtime.exe` | Game runtime host: Loads the project manifest (`project.we.yaml`) and packaged assets; lightweight standalone execution without editor UI overhead |
| `Launcher/` | `WeEdit.exe` / `WePlay.exe` | Standalone zero-dependency project launchers (Edit / Play mode) for one-click launching from any external project directory |
| `plugins/` | Plugin dynamic libraries | Extensible engine plugin ecosystem (custom asset types, components, editor panels, and Lua libraries) |
| `tests/` | CTest regression suite | Engine test suite (78 tests, including schema drift, runtime contracts, component layouts, Luau sandbox/bindings, and Level/Prefab 2.0) |

Dependency direction: Hosts (`Editor`, `Runtime`, `Launcher`) and `Game` depend on `World`; `World` never depends back on a host. Modules register across boundaries through explicit interfaces without relying on static initialization order.

WorldEngine also supports **subproject mode** (`WORLD_AS_SUBPROJECT=ON`): external game projects can include the engine directly via `add_subdirectory(<engine_root>)`.

## Key Features & Architecture

- **Level 2.0 & Prefab 2.0 Authoring**:
  - *Level 2.0*: Decoupled level format pairing an entry manifest (`scenes/<Id>.wlevel`) with per-entity documents (`levels/<Id>/entities/*.entity`). Monolithic `.wd` format is fully retired. Supports Data Layers with editor layer management and runtime Luau streaming APIs.
  - *Prefab 2.0*: Self-contained single-file `.wprefab` assets with stable `PrefabLocalId` mapping, inline property overrides, and zero loose child files.
  - *OFPA Sparse Placement*: Instances serialize only template references and delta overrides, ensuring compact storage and robust serialization roundtrips.
- **Modern RHI & Slang Shading Pipeline**:
  - Unified hardware abstraction layer supporting **OpenGL 4.6** (default) and **Vulkan** backends (configured via `project.we.yaml`).
  - Powered by the **Slang** compiler CLI (`slangc`), translating `*.slang` sources into dual-target SPIR-V with parameter reflection annotations and build-time shader caching.
- **Embedded Luau Scripting Runtime**:
  - Sandboxed Luau VM with schema-driven thin bindings and live hot-reloading for gameplay systems and components.
  - Automatic, deterministic generation of `WorldEngineAPI.luau` language server completion stubs in `<project>/assets/scripts/intermediate/`.
- **Schema-Driven ECS & Retained WUI**:
  - C++ components expose metadata via annotations; schema compiler generates typed serialization accessors and drives editor property inspectors automatically.
  - Pure retained WUI UI framework (100% components registered, 0 raw draw violations) shared between Editor shell/panels and in-game HUDs.
  - Viewport 2.0 featuring Perspective and Orthographic camera projections with adaptive ruler step, orientation gizmo, and 1500m forward-biased multi-LOD ground grid.
- **Physics & High-Performance Math**:
  - 2D (Box2D) and 3D (Jolt Physics) integrated into the ECS with collision event dispatching.
  - Math library with white-listed SIMD optimizations strictly adhering to throughput gates.
- **Project Isolation & Scaffolding**:
  - Project launcher and wizard allow scaffolding games anywhere outside the engine checkout using templates (`templates/project-empty`, `templates/project-example`).

## Quick Start

### Prerequisites

- **OS**: Windows 10 / 11 (x64)
- **Compiler**: Visual Studio 2022 or 2026 (with "Desktop development with C++" workload)
- **CMake**: ≥ 3.16
- **Vulkan SDK**: Headers and import libraries are required during CMake configuration (regardless of active backend)

### 1. Configure

```powershell
# Debug / x64 configuration with test target enabled
cmake -S . -B build/x64-Debug -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_BUILD_TYPE=Debug -DWORLD_BUILD_SCRIPT_TESTS=ON

# (Replace generator with "Visual Studio 18 2026" or another generator supported on your machine)
```

### 2. Build

```powershell
# Build all targets (Editor, Runtime, Game, Launchers, tests)
cmake --build build/x64-Debug --config Debug --target ALL_BUILD --parallel

# Or build the Editor only (automatically builds Runtime, Game, and World)
cmake --build build/x64-Debug --config Debug --target Editor --parallel
```

### 3. Test

```powershell
# Run the full regression test suite (78 tests)
ctest --test-dir build/x64-Debug -C Debug --output-on-failure

# Alternatively, run via CMake target (requires building ALL_BUILD first)
cmake --build build/x64-Debug --config Debug --target RUN_TESTS
```

### 4. Run

```powershell
# Working directory = repository root

# 1) Launch Editor
# Without arguments, opens the Project Launcher / Wizard to create or open projects:
.\build\x64-Debug\Editor\Debug\Editor.exe

# Or specify a project directly:
.\build\x64-Debug\Editor\Debug\Editor.exe --project <Path-To-Project>

# 2) Launch standalone Runtime
.\build\x64-Debug\Runtime\Debug\Runtime.exe --project <Path-To-Project>

# 3) Standalone Project Launchers
# Copy WeEdit.exe / WePlay.exe into any project folder and double-click to launch:
.\build\x64-Debug\Launcher\Debug\WeEdit.exe
.\build\x64-Debug\Launcher\Debug\WePlay.exe
```

### Build & ABI Notes

- **PDB Write Serialization (`/FS`)**: Parallel MSVC builds compile with `/FS` to prevent multi-process PDB write lock conflicts (`error C1041`).
- **Explicit `WLD_API` DLL Boundary**: `WorldRuntime.dll` exports public symbols strictly through `WLD_API` with precompiled headers (PCH) enabled, avoiding `WINDOWS_EXPORT_ALL_SYMBOLS` link overhead and 64k symbol table overflow.
- **Runtime Layout Match Guard**: `WE_RUNTIME_LAYOUT_MATCHES()` validates struct sizes and offsets (`sizeof(Scene)` / `sizeof(WorldContext)`) between host binaries and the core DLL at startup (exit code 3 on mismatch). If encountered, perform a clean full rebuild.
- **Config Consistency**: `CMAKE_BUILD_TYPE` and `--config` must match (`Debug` for both or `Release` for both). Re-configure CMake after adding or removing source files.

## Repository Layout

| Path | Description |
| --- | --- |
| `Engine/src/World/Core/` | Application lifecycle, memory allocators, jobs, VFS, logging, project manifest |
| `Engine/src/World/Platform/` | Windows platform layer: Window management, raw input, dynamic library loading |
| `Engine/src/World/RHI/` | Rendering hardware interface with OpenGL 4.6 and Vulkan backends |
| `Engine/src/World/Scene/` | Flecs-based ECS, Level 2.0 data layer manager, scene serialization |
| `Engine/src/World/Schema/` | Schema reflection annotations and code generators |
| `Engine/src/World/WUI/` | Retained WUI widget system, layout engine, accessibility tree (shared by Editor & Runtime HUD) |
| `Engine/src/World/Script/` | Luau VM, sandboxed bindings, hot reload, language server stub generator |
| `Engine/src/World/Physics/` | 2D (Box2D) and 3D (Jolt) physics simulations and ECS integration |
| `Engine/src/World/Math/` | Linear algebra, math primitives, and SIMD hardware acceleration |
| `Engine/assets/shaders/` | Slang shader sources (`*.slang`, entry points `VSMain` / `PSMain`) |
| `Editor/src/WUI/` | Editor shell, panels (Hierarchy, Inspector, Viewport, Levels, Data Layers, Content Browser) |
| `Runtime/` | Standalone game client host (zero editor UI overhead) |
| `Launcher/` | Zero-dependency standalone project launchers (`WeEdit`, `WePlay`) |
| `Game/src/` | Game DLL source (`Game.dll`) connecting via explicit ABI registration |
| `plugins/` | Extensible plugin modules |
| `templates/` | Scaffolding templates: `project-empty`, `project-example`, `plugin-*` |
| `projects/` | Local workspace directory for projects kept inside checkout (git-ignored) |
| `tests/` | 78 domain regression tests registered with CTest |
| `tools/agents/` | Environment preflight, architecture layout checks, SIMD gates, WUI linters |
| `docs/` | Public user guides and developer documentation |

## Asset & File Formats

| Extension / Format | Purpose | Description |
| --- | --- | --- |
| `project.we.yaml` | Project Manifest | Project settings: Content root, startup scene, renderer (`opengl` / `vulkan`), physics settings |
| `.wlevel` | Level Entry | Level manifest (`scenes/<Id>.wlevel`): Level ID, name, world bounds, and Data Layer configurations |
| `.entity` | Entity Document | Individual entity file (`levels/<Id>/entities/*.entity`): Contains `ChunkID` and `Entity` definition |
| `.wprefab` | Prefab 2.0 Asset | Self-contained entity subtree template (`FormatVersion: 2`) with stable `PrefabLocalId` and inline overrides |
| `.wmodel` | Model Asset | Processed 3D model asset generated from imported `.gltf` / `.glb` sources |
| `.wmat` | Material Asset | Material instance (`FormatVersion: 3`) referencing Slang shaders with texture and parameter overrides |
| `.slang` | Shader Source | Slang shader files compiled to dual-target SPIR-V for OpenGL and Vulkan |
| `.wpak` | Content Package | Cooked VFS archive for game distribution |
| `.luau` / `.lua` | Script | Embedded gameplay scripts running inside the Luau sandboxed runtime |
| `WorldEngineAPI.luau` | IDE Stubs | Auto-generated type definitions for Luau LSP (`<project>/assets/scripts/intermediate/`) |

> Note: The legacy single-file `.wd` scene format is deprecated and completely retired in favor of the decoupled Level 2.0 format.

## Documentation

| Guide | Location | Topics |
| --- | --- | --- |
| User Manual | [`docs/user/`](docs/user/) | Getting started, Editor workflows, Projects, Assets, Packaging, FAQ |
| Developer Architecture | [`docs/dev/`](docs/dev/) | Architecture overview, Build and run, Module contracts, WUI, Slang shaders |
| Scripting Guide | [`docs/user/scripting/README.md`](docs/user/scripting/README.md) | Luau scripting runtime, Entities & systems, Lua language tooling |
| Project Standard | [`docs/dev/project-standard.md`](docs/dev/project-standard.md) | Standardized project structure and scaffolding specifications |

## Quality Gates & Verification

WorldEngine enforces automated quality gates in `tools/agents/`:

```powershell
# Environment & toolchain preflight check
& .\tools\agents\check-environment.ps1

# Include root & repository layout conformance
& .\tools\agents\check-layout.ps1

# WUI component registration & raw draw zero-tolerance verification
& .\tools\agents\check-ui-components.ps1

# SIMD performance throughput policy gate
& .\tools\agents\check-simd.ps1

# Project structure conformance check
& .\tools\agents\check-project-layout.ps1 -ProjectRoot <Path-To-Project>
```

## Repository

- **Source**: [https://github.com/AdrianJLee/WorldEngine](https://github.com/AdrianJLee/WorldEngine)
- **Default Branch**: `main` (active development takes place on `develop`)
- **License**: Apache-2.0, see [`LICENSE`](LICENSE) and [`NOTICE`](NOTICE)
