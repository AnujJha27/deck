# deck

Fresh C++ reboot scaffold for the `deck` workstation MVP described in the handoff plan.

## Project tracking docs

- `VISION.md` explains the product target and why `deck` exists.
- `ARCHITECTURE.md` describes the current system boundaries and near-term technical direction.
- `MVP_PLAN.md` defines the target scope for the first usable version.
- `RECOVERED_PLAN.md` captures the earlier Codex planning thread recovered from local history.
- `IMPLEMENTATION_STATUS.md` records what is already built versus still scaffolded.
- `ROADMAP.md` tracks the next implementation slices as a checklist.
- `BACKLOG.md` keeps non-immediate work visible without bloating the roadmap.
- `DECISIONS.md` records confirmed product and engineering decisions.

## Current shape

This repository now contains:

- a compileable C++20 core library
- a real `FTXUI` fullscreen shell with tab switching and pane-shaped split rendering
- CLI entry points for `deck`, `deck doctor`, `deck workspace open`, `deck workspace list`, `deck workspace reset-layout`, and `deck --safe`
- persisted multi-tab workspace state with per-tab split layouts and focused panes
- SQLite-backed workspace state storage under `.deck/state.db`
- a typed event bus
- environment capability detection
- a non-PTY process runner for safe argv-based command execution
- a lightweight `:` command palette for run/search/git/finance actions
- review-tab Git actions for refresh, file selection, stage/unstage, selected-file diff, and commit message entry
- context-linked notes for selected files, search results, and finance symbols, stored under `.deck/notes/`
- a dedicated Notes tab that combines the context-note view with a workspace-local scratchpad backed by `.deck/scratch.md`
- finance workspace scaffolding with watchlist and local data-source discovery
- header-driven CSV ingestion for positions and balances
- a portfolio summary with tracked totals, cash, market value, and daily change when quotes are available
- legacy SQLite schema for papers, paper tags, and paper bookmarks still present during the product pivot
- unit-style tests for split tree persistence, workspace persistence, event routing, and `PaperAnchor` serialization

## What is still scaffolded

PTY task execution and richer finance analytics/provider support are not fully implemented yet. The core abstractions are in place so those subsystems can be added without reworking the workspace model.

## Notes

Context-linked notes now follow the current file, search result, or market symbol selection:

- `E` starts editing the current context note from any tab
- `Ctrl+S` saves the current context note under `.deck/notes/`
- `Ctrl+R` reloads the current context note from disk
- `Esc` leaves note edit mode

Inside the Finance tab, `e` is a shortcut for editing the selected symbol note.

## Scratchpad

The dedicated `Notes` tab also provides a workspace-local scratchpad:

- `e` starts inline editing
- `Ctrl+S` saves to `.deck/scratch.md`
- `Ctrl+R` reloads from disk
- `Esc` leaves edit mode

## Finance API setup

For live quote refresh in the Finance tab, either export:

```bash
export FINNHUB_API_KEY=your_key_here
```

or place this in the `deck` launch directory `.env` file:

```bash
FINNHUB_API_KEY=your_key_here
```

Inside the Finance tab:

- `a` opens ticker input and persists it to `.deck/watchlist.txt`
- `x` refreshes quotes when no task is running
- local `.csv` files with headers like `symbol,quantity,cost_basis_total` or `account,amount,currency` are discovered automatically

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```
