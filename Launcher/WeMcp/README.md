# WorldEngine MCP Suite (WeMcp)

WorldEngine MCP Suite is an industrial-grade Model Context Protocol (MCP) server for WorldEngine, providing AI assistants (Claude, Codex, Cursor, Windsurf, Cline) with high-throughput authoring and verification capabilities.

## Source & Module Layout

```text
Launcher/WeMcp/
├── CMakeLists.txt                # Build configuration (outputs WeMcp.exe & we-mcp.exe)
├── README.md                     # Documentation
├── setup-mcp.ps1                 # One-click client auto-configurator
├── src/                          # Server source code (native C++17)
│   ├── main.cpp                  # stdio JSON-RPC entrypoint
│   ├── EngineBridge.h / .cpp     # Self-locating IPC bridge to Editor host / headless Job Object
│   ├── McpServer.h / .cpp        # Tool groups, batch execution, and MCP resources
│   └── McpProtocol.h             # JSON-RPC protocol types
├── templates/                    # Ready-to-use client configurations
│   ├── cursor-mcp.json           # Cursor configuration
│   ├── claude_desktop_config.json # Claude Desktop configuration
│   └── codex-mcp.json            # Codex CLI configuration
└── skills/                       # Accompanying AI agent skill
    └── SKILL.md                  # Mental model, guidelines, and quality gates
```

## Quick Start (One-Click Setup)

Run `setup-mcp.ps1` in PowerShell to automatically configure detected AI clients:

```powershell
# Configure all installed clients (Cursor, Claude Desktop, Codex, Windsurf, Cline)
.\Launcher\WeMcp\setup-mcp.ps1

# Configure specific client
.\Launcher\WeMcp\setup-mcp.ps1 -TargetClient cursor
```

## Core Ergonomics & Features

### 1. High-Throughput Batch Execution (`batch_execute`)
Eliminates multi-turn LLM roundtrip latency by executing multiple commands in a single request:
```json
{
  "tool": "batch_execute",
  "arguments": {
    "commands": [
      { "tool": "scene.create_entity", "params": { "name": "Player", "location": "0,0,0" } },
      { "tool": "scene.add_component", "params": { "name": "Player", "component": "CameraComponent" } },
      { "tool": "scene.add_component", "params": { "name": "Player", "component": "CharacterControllerComponent" } }
    ],
    "fail_fast": true
  }
}
```

### 2. Dynamic Tool Groups (`manage_tools`)
To minimize context token consumption, 70+ tools are divided into logical groups. Default active groups: `core`, `scene`, `asset`, `playtest`.

- `manage_tools(action="list_groups")`: Inspect group status.
- `manage_tools(action="activate", group="vfx")`: Dynamically expose VFX tools and notify client.
- `manage_tools(action="deactivate", group="vfx")`: Hide domain tools.

Available groups: `core`, `scene`, `asset`, `playtest`, `vfx`, `audio`, `navmesh`, `ui`, `profiling`.

### 3. Native MCP Resources (Zero-Turn Context)
Inspect engine capabilities and schemas without spending tool-call turns:
- `worldengine://project/info`: Engine version, project root directory, host mode.
- `worldengine://schemas`: Registered ECS component schemas.
- `worldengine://api/stubs`: Complete `WorldEngineAPI.luau` scripting stubs.
- `worldengine://tool-groups`: Catalog of active and available tool groups.

### 4. Zero-Dependency & Self-Healing Lifecycle
- Pure native C++17 single-bundle architecture. No Python, uv, Node.js, or npm required.
- Self-locating relative path resolution.
- Automatically connects to running `Editor.exe` (port 8089) or launches `--headless` host inside a Windows Job Object (`KILL_ON_JOB_CLOSE`) to ensure zero orphaned background processes.
