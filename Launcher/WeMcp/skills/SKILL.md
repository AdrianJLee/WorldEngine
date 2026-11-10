---
name: worldengine-mcp
description: "Control WorldEngine (C++17 native engine) via Model Context Protocol: manage scenes, entities, components, assets, Luau scripts, and playtests with high throughput batch_execute and dynamic tool groups."
---

# WorldEngine MCP Skill

This skill explains how to effectively operate WorldEngine through its Model Context Protocol (MCP) server (`we-mcp.exe`).

## 1. Architecture & Protocol
- **Transport**: Standard IO (JSON-RPC 2.0).
- **Binary**: Native C++ executable located at `tools/we-mcp/bin/we-mcp.exe` (accompanied by `WorldRuntime.dll`).
- **Bridge**: Automatically connects to the interactive `Editor.exe` (port 8089) or auto-spawns `--headless` Editor inside a Windows Job Object.

## 2. High-Throughput Operations (`batch_execute`)
Never issue multiple back-and-forth roundtrips to create and configure an entity. Bundle them into a single `batch_execute` call:

```json
{
  "tool": "batch_execute",
  "arguments": {
    "commands": [
      { "tool": "scene.create_entity", "params": { "name": "Boss", "location": "0,0,0" } },
      { "tool": "scene.add_component", "params": { "name": "Boss", "component": "HealthComponent" } },
      { "tool": "scene.set", "params": { "name": "Boss", "component": "HealthComponent", "property": "MaxHealth", "value": "1000" } }
    ],
    "fail_fast": true
  }
}
```

## 3. Dynamic Tool Groups (`manage_tools`)
To minimize prompt tokens, tools are grouped into domains:
- `core`: session, workflow, changeset, project, manage_tools, batch_execute (always active).
- `scene`: scene.*, schema.*, state.dump, stats.scene (active by default).
- `asset`: asset.*, material.*, datatable.compile (active by default).
- `playtest`: playtest.run, play.*, capture.* (active by default).
- `vfx`: particle systems and VFX tools.
- `audio`: sound events, combo pitches, audio buffer capture.
- `navmesh`: 3D navigation mesh baking, queries, raycasts.
- `ui`: WUI accessibility tree and virtual inputs.
- `profiling`: memory attribution and frame profiler stats.

Activate a domain group on demand:
```json
{
  "tool": "manage_tools",
  "arguments": { "action": "activate", "group": "vfx" }
}
```

## 4. Native MCP Resources (Zero-Turn Context)
Read capabilities directly without executing tool calls:
- `worldengine://project/info`: Engine version, project root directory, and connection status.
- `worldengine://schemas`: Catalog of registered ECS components and fields.
- `worldengine://api/stubs`: Complete `WorldEngineAPI.luau` stubs for script synthesis.
- `worldengine://tool-groups`: Catalog of active and available tool groups.

## 5. Authoring Pipeline & Quality Gates
1. **Explore**: Read `worldengine://project/info` and `worldengine://schemas`.
2. **Batch Author**: Use `batch_execute` to create entities, attach components, and configure values. Save with `scene.save`.
3. **Audited Scripts**: Always write Luau scripts via `changeset.begin` -> `changeset.stage_write` -> `changeset.apply`.
4. **Deterministic Playtest**: Validate with `playtest.run` specifying frame step count and invariants (`no_void_fall`, `no_nan`, `entity:<Name> exists`).
