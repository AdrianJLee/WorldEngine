# WorldEngine

English | [Simplified Chinese](README.zh-CN.md)

A native Windows game engine: the engine core builds as the shared library **`WorldRuntime.dll`**,
game logic builds as a separate **`Game.dll`**, and the editor and runtime are host programs.
C++17 + CMake, with third-party dependencies locked into the repository (`vendor/`,
`third_party/` and submodules).

## Modules

| Module | Artifact | Responsibility |
| --- | --- | --- |
| `World/` | `WorldRuntime.dll` | Engine core: application and memory, jobs, VFS and assets, rendering RHI (OpenGL / Vulkan), scene and reflection schema, WUI framework, scripting runtime (Luau) |
| `Game/` | `Game.dll` | Game logic. A separate DLL that plugs into the engine through **explicit registration interfaces**; it never duplicates engine singletons |
| `Editor/` | `Editor.exe` | Editor host: viewport, scene hierarchy, properties, content browser, material / model / prefab panels, Play |
| `Runtime/` | `Runtime.exe` | Game host: loads the project manifest and content, no editor UI |
| `tests/` | ctest suite | Engine regression tests (not built by default, see below) |

Dependency direction: the hosts (`Editor` / `Runtime`) and `Game` depend on `World`; `World` never
depends back on a host. Scenes, components, assets and scripts register across module boundaries
through explicit interfaces — no reliance on static initialization order, and no assumed
singletons shared across modules.

## Quick start

Prerequisites: Windows, Visual Studio (with the "Desktop development with C++" workload),
CMake ≥ 3.16, and the Vulkan SDK (headers and import libraries are required at configure time
even though the default rendering backend is OpenGL).

```powershell
# 1) Configure (Debug / x64; add -DWORLD_BUILD_SCRIPT_TESTS=ON to get the RUN_TESTS target)
cmake -S . -B build/x64-Debug -G "Visual Studio 18 2026" -A x64 `
      -DCMAKE_BUILD_TYPE=Debug -DWORLD_BUILD_SCRIPT_TESTS=ON

# 2) Build (Editor pulls in Runtime and Game; use --target World for the engine only)
cmake --build build/x64-Debug --config Debug --target Editor --parallel

# 3) Test: build ALL_BUILD first, then run RUN_TESTS
#    RUN_TESTS only drives ctest and does not rebuild the test binaries, so the reverse
#    order would test stale binaries against freshly built DLLs.
cmake --build build/x64-Debug --config Debug --target ALL_BUILD
cmake --build build/x64-Debug --config Debug --target RUN_TESTS

# 4) Run (working directory = repository root)
.\build\x64-Debug\Editor\Debug\Editor.exe
.\build\x64-Debug\Runtime\Debug\Runtime.exe
```

- Replace the generator name with one listed by `cmake --help` on your machine; this repository
  is verified with `Visual Studio 18 2026`.
- `CMAKE_BUILD_TYPE` and `--config` must match (`Debug` for both or `Release` for both),
  otherwise the artifact paths will not line up.
- Re-configure after adding or removing source files: source lists are collected at configure
  time (there is no `CONFIGURE_DEPENDS`).

## Repository layout

| Path | What it is |
| --- | --- |
| `Engine/src/World/Core/` | Application, memory, jobs, VFS, project manifest |
| `Engine/src/World/Scene/` | ECS, scene serialization, component definitions |
| `Engine/src/World/Schema/` | Reflection schema and generated accessors |
| `Engine/src/World/RHI/` | Rendering frontend with OpenGL / Vulkan backends |
| `Engine/src/World/WUI/` | WUI widgets and layout framework (shared by the editor and the Runtime HUD) |
| `Engine/src/World/Script/` | Luau runtime, bindings and hot reload |
| `Engine/assets/shaders/` | HLSL shader sources (`*.hlsl`, entry points `VSMain` / `PSMain`) |
| `Game/src/` | Gameplay DLL source (`Game.dll`); the project content root lives under `projects/` |
| `projects/default/` | Default project content root: `project.we.yaml` plus `assets/scenes/` and `assets/scripts/` |
| `Editor/src/WUI/` | Editor panels and shell |
| `tests/` | Per-topic test executables and ctest registration |
| `docs/` | Public documentation (see below) |

## Documentation

| Audience | Location |
| --- | --- |
| Game developers | [`docs/user/`](docs/user/) — getting started, editor, assets, scripting, packaging, FAQ |
| Engine / build contributors | [`docs/dev/`](docs/dev/) — build and run, architecture, extension points |
| Scripting and code completion | [`docs/scripting/lua-tooling.md`](docs/scripting/lua-tooling.md) |

## Asset and script types

| Extension | What it is |
| --- | --- |
| `.wd` | Scene file (entities and components) |
| `.wmodel` | Model asset: produced by importing `.gltf` / `.glb`; import settings live in the asset |
| `.wmat` | Material asset |
| `.wprefab` | Prefab |
| `.wpak` | Content package (packaging output) |
| `.lua` / `.luau` | Scripts; the embedded scripting runtime is Luau |
| `WorldEngineAPI.luau` | Editor-generated completion stubs (`projects/default/assets/scripts/intermediate/`); do not edit by hand |

## Repository

- Source: https://github.com/AdrianJLee/WorldEngine
- Default branch: `main` (development happens on `develop`)
- License: Apache-2.0, see [`LICENSE`](LICENSE)
