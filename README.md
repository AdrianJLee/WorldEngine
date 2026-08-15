# WorldEngine

## Overview
WorldEngine is a modular C++ game and graphics engine framework. The repository separates the engine core (World) provided as a static library (World.lib) from game code (Game) built as a separate target. The project intentionally keeps World as a static library while providing explicit extension and registration points so Game can register plugins, factories, or callbacks across the static-library boundary without converting World into a DLL.

## Features
- Modular structure: World (engine core), Game (game logic), and other subsystems
- CMake-based build system with Visual Studio generator support
- Explicit registry/registration pattern to enable safe cross-target extension while keeping World statically linked
- Primary C++ standard: C++17 (compatibility headers/code may exist for older standards where required)

## Repository
- Source: https://github.com/AdrianJLee/WorldEngine
- Default branch: main

## Requirements
- CMake >= 3.16 (tested with 4.3.1-msvc1)
- Microsoft Visual Studio 2026 (MSVC toolchain)
- x64 build recommended
- Preferred C++ standard: C++17

Prefer target-based linking (target_link_libraries) so include paths, compile options and transitive dependencies are propagated correctly.

## Cross-boundary registration & singleton strategy (recommended)

Goal: Allow Game to register plugins, factories, or callbacks with World while keeping World as a static library and avoiding static initialization order issues or duplicated singletons across the static-library / executable boundary.