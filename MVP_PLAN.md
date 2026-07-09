# MVP Plan

Last updated: 2026-07-10

## Goal

Ship a disciplined `deck` MVP: a terminal-native C++ `FTXUI` workstation for software engineering and finance monitoring, with persistent multi-tab workspaces and enough workflow depth to feel like a real daily driver.

## Product shape

The MVP is meant to be:

- a real SWE workstation rather than a toy shell
- more structured than `tmux`, but not a full terminal multiplexer replacement
- strong in dev/review/search/log flows
- useful for lightweight project thinking and capture inside the workspace
- useful as a personal watchlist and local portfolio-monitor surface

## In scope

### Workspace shell

- Fullscreen `FTXUI` shell
- Multi-tab workspaces
- Per-tab split trees and focused pane persistence
- Safe-mode startup and recovery path
- Workspace state under `.deck/`
- `deck doctor`

### Dev workflow

- `Dev`, `Run`, `Review`, and `Finance` tabs
- Files pane rooted at the workspace
- One active task model per workspace
- Runtime task history, exit status, and output summaries
- Git branch/status and diff visibility in the UI
- Search pane backed by workspace-aware discovery, then `rg`
- Logs pane grounded in task output and configured sources
- Context-linked Notes pane for file/search/finance sidecar notes
- Dedicated Scratch pane for general workspace capture

### Finance workflow

- Markets pane with a watchlist loaded from `.deck/watchlist.txt` or defaults
- Portfolio pane with focused-symbol context and local finance data-source discovery
- Finance context should be able to drive linked notes
- Local CSV/SQLite/JSON discovery to seed future portfolio ingestion

## Deferred from MVP

- full PTY task terminal
- inline Git staging and commit UI
- interactive search result navigation
- inline linked-note and scratch editing
- multiple simultaneous interactive PTYs
- live quote providers and alerts
- plugin/runtime extension system
- removal of legacy paper persistence internals

## Quality bar

- `FTXUI` owns the shell experience cleanly
- pane content reflects real workspace/runtime state rather than placeholders
- the app boots even when persisted UI state is bad
- capabilities degrade clearly based on the environment
- `cmake --build build` passes
- `ctest --test-dir build` passes
