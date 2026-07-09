# Architecture

Last updated: 2026-07-10

## Core structure

The current architecture is organized around a small C++20 core:

- `FTXUI` shell and layout rendering
- workspace persistence and split-tree layout model
- typed event bus
- process subsystem for external tools
- SQLite-backed state storage
- pane abstraction for feature surfaces

## Key boundaries

### Persistent vs runtime state

Persistent state belongs in `WorkspacePersistentState`:

- tabs and tab order
- split layouts
- focused tab/pane
- selected log source
- recent commands
- durable workspace metadata

Runtime state belongs in `WorkspaceRuntimeState`:

- active task state
- task history and output summaries
- transient pane results
- active note context, scratch buffer, and editor state
- finance watchlist focus and discovered sources
- future async jobs and overlay handles

### Pane model

Every pane implements the shared `Pane` interface and should be renderable without directly reaching into other panes. Cross-feature coordination should happen through shared state and the event bus.

Notes and scratch should stay separate at the product level:

- linked notes react to file/search/finance context changes
- scratch is a stable workspace-local free-form document
- both can share editor primitives without sharing identity or persistence rules

### Process execution

All external commands should flow through `ProcessRunner` so argv safety, cwd handling, timeouts, and output capture stay consistent across Git, search, editor handoff, task execution, and future finance import/update jobs.

## Current simplifications

- panes are still static renderers over runtime snapshots
- there is no PTY subsystem yet
- async jobs and generation-id invalidation are not wired yet
- finance panes are still local-data scaffolding rather than full analytics surfaces
- notes are still summary-only rather than editable linked documents
- legacy paper persistence remains in the codebase but is no longer part of the active UI

## Near-term architecture direction

- formal task runtime model
- refreshable pane data instead of one-shot snapshots
- command/event surfaces for file, git, and search actions
- note and scratch persistence/editing flow
- finance ingestion pipeline for CSV and SQLite sources
