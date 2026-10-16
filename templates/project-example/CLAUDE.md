# WorldEngine Game Project — AI Agent Guidelines (AGENTS.md)

1. **Channel Enforcement**: All project modifications MUST go through `WeMcp` (`we-mcp.exe`) or the in-editor AI Assistant channel so `WriteGuard` and `ChangesetTracker` record attribution in `.we/ai/writes/`.
2. **Session & Role Isolation**: Call `session.open` with your assigned role (`producer`, `gameplay_programmer`, `2d_artist`, `qa_lead`, etc.) before executing write or exec commands.
3. **Self-Validation First**: Run `project.validate`, `asset.validate`, and `playtest.run` before submitting tasks for review (`review.submit`). Self-review (`reviewer == task.role`) is strictly forbidden.
4. **Engine Boundary**: Never modify the WorldEngine source tree (`Engine/`, `Editor/`, `Runtime/`). Report engine gaps via `engine.request_change`.
