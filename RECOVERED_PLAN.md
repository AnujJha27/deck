# Recovered Plan

Recovered on: 2026-07-07

Source: local Codex terminal history from sessions `019f334a-2497-7223-a44b-f2b56d47264a` and `019f33a5-f4c8-75c2-a008-d9a191bc8222`.

## Original product intent

`deck` is meant to be a terminal-native software engineering workstation, closer to "tmux on steroids" than a minimal TUI demo. The ambition comes from a coherent workspace runtime with panes, tabs, command routing, persistent layouts, and strong interoperability between dev and research workflows.

The app should feel like a serious daily driver for:

- software engineering work
- repo review
- task execution
- log/search workflows
- paper reading and research notes

Finance and market-monitor features were part of the earlier broad vision, but they were explicitly pushed out of the current MVP.

## Recovered vision

The earlier thread described the strongest version of the product as a terminal operating environment with:

- a persistent workspace model
- panes and tabs that coordinate with each other
- a command palette as the central control surface
- file, git, terminal, log, search, and data tools that react to shared events

The core idea was not "many unrelated modules", but a shared runtime where:

- selecting a repo retargets git and terminal panes
- logs follow active project context
- notes attach to workspace state
- research assets and reading position persist cleanly

## Recovered ambitious MVP

The later planning thread narrowed that broader vision into a C++ + `FTXUI` MVP with these priorities:

### Shell and workspace

- Fullscreen `FTXUI` shell
- Multi-tab workspaces
- Persisted per-tab split trees
- Focused pane persistence
- Safe-mode launch and recovery path
- `deck doctor`

### Dev workflow

- Files pane rooted at workspace directory
- One active task terminal per workspace
- Task history, rerun, cancel, and buffered output
- Git pane with branch/status summary
- Diff visibility
- Search pane backed by `rg`
- Logs pane for task output and configured file tails

### Research workflow

- Paper library pane with local PDF discovery
- Paper reader with rendered-page mode when graphics support exists
- Extracted-text fallback
- Notes pane for research markdown
- Durable paper anchors and persisted reading state

### Explicit deferrals

- Git push UI
- full tmux-grade multiplexing
- multiple simultaneous interactive PTYs
- finance modules
- Docker and `journalctl` log integrations
- plugin system

## Recovered architecture direction

The planning thread consistently emphasized these design constraints:

- Use `FTXUI` properly as the UI shell rather than bolting on a weak renderer.
- Keep a typed event bus at the center of cross-pane communication.
- Separate persistent state from runtime state.
- Route all external tool execution through a shared process subsystem.
- Degrade cleanly based on environment capabilities instead of scattering ad hoc checks.
- Treat async work as first-class and defend against stale results with generation ids.

## Recovered research-reader direction

The earlier chat also captured a specific reader strategy:

- render real PDF pages using external tools rather than inventing a fake PDF widget
- prefer Kitty graphics for rendered-page quality
- keep extracted-text mode for search, fallback, and low-capability terminals
- reserve pane geometry in `FTXUI`, but treat image placement as a separate overlay concern

That led directly to the current `PaperAnchor` and environment-capability direction in the codebase.

## Product stance recovered from the thread

These points came through repeatedly and are worth preserving:

- This should be a real SWE workstation, not a toy shell.
- The MVP should still be disciplined; ambition should come from workflow depth, not random subsystem sprawl.
- `FTXUI` is the correct shell technology for this reboot.
- The product should boot fast, persist state well, and recover from bad UI state.
- The repo should track plan, status, and roadmap as first-class project documents.

## Relationship to current docs

This file is the recovered historical intent.

The authoritative working docs for the current repo state are:

- `MVP_PLAN.md`
- `IMPLEMENTATION_STATUS.md`
- `ROADMAP.md`

If those diverge from this recovered plan, treat this document as product history and the other docs as the current implementation contract.
