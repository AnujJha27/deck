# Implementation Status

Last updated: 2026-07-10

## Completed

- C++20 core library and `deck` executable are building.
- `FTXUI` fullscreen shell is live with tab switching and split rendering.
- Workspace persistence is implemented, including tab layouts and focused panes.
- SQLite-backed workspace storage is wired under `.deck/state.db`.
- Typed event bus is implemented.
- Environment capability detection is implemented.
- Non-PTY subprocess execution is implemented.
- Runtime task records now capture command name, exit state, timestamps, and output excerpts for startup probes.
- The shell can launch a recent command, rerun the latest task, cancel the active task, and stream live subprocess output into runtime task records.
- The Search pane now supports in-app `rg` queries with bounded async results and basic result selection.
- Selected search results can now open safely in `nvim` with line targeting through restored terminal I/O.
- The Files pane now supports basic keyboard navigation and file-open handoff into `nvim`.
- The fourth tab has been repurposed from research into a Finance workspace.
- Finance panes now render a watchlist, a focused symbol, and discovered local CSV/SQLite/JSON data sources instead of placeholder text.
- Finance watchlists can now be extended in-app and persisted to `.deck/watchlist.txt`.
- Direct API-backed quote refresh is now wired through Finnhub when `FINNHUB_API_KEY` and `curl` are available.
- Finance CSV ingestion now loads positions and balances from header-driven local `.csv` files into runtime state.
- The Portfolio pane now summarizes tracked positions, balances, cost basis, cash, market value, and daily change from available quotes.
- Market refresh now supports pluggable local/external sources by preferring local CSV quotes and falling back to Finnhub when available.
- A lightweight `:` command palette now routes common actions like run/rerun, search, git status/diff, refresh, and finance watchlist commands through the existing shell actions.
- Review tabs now keep a live parsed Git file list with selected-file diff rendering, stage/unstage shortcuts, a commit-message overlay, and palette equivalents for basic commit flow actions.
- Context-linked notes are now available for selected files, search results, and finance symbols, with note files persisted under `.deck/notes/`.
- A dedicated Notes tab now combines the context-note surface with a workspace scratchpad backed by `.deck/scratch.md`, both with inline editing, save, and reload controls.
- The shell footer now exposes status messaging for recent actions and degraded behavior.
- Pane rendering now uses cached snapshots instead of recomputing workspace scans and Git summaries on each redraw.
- `PaperAnchor` serialization and the legacy paper schema remain in persistence for now, but they are no longer part of the active MVP surface.

## Partially implemented

- Terminal and Logs panes reflect real runtime task records and live non-PTY task streaming, but not a PTY session.
- Search pane can run and display `rg` queries and open selected results in `nvim`, but broader file navigation is still missing.
- File navigation is keyboard-driven and flat, but there is no true tree browser yet.
- Git and Diff panes now support basic file-level staging, unstaging, selected-file diff inspection, and commit submission, but not hunk-level selection or branch management.
- The command palette covers core actions, but it is still a typed overlay rather than a richer fuzzy picker or full command router.
- Finance panes have quote refresh, CSV ingestion, and portfolio summary metrics, but alerting is still missing.
- The shell has multi-tab structure and persistence, but tab-local interactivity remains thin.

## Not implemented yet

- PTY task execution.
- Live task log streaming.
- Interactive file browser actions.
- Diff navigation.
- Alerts and richer portfolio analytics.

## Known issues

- Quitting the shell can still leave terminal mouse/escape sequences printed to the screen in some cases, which means terminal state restoration is not fully correct yet.
- The UI can still feel laggy when switching tabs or forcing repeated redraws.

## Current MVP assessment

The shell, persistence model, and pane framework are now beyond scaffold level. The main missing work is interactivity: deeper dev workflow actions, lower-latency rendering behavior, and real finance data ingestion.
