# Implementation Status

Last updated: 2026-07-17

## Completed

- C++20 core library and `deck` executable are building.
- `FTXUI` fullscreen shell is live with tab switching and split rendering.
- Workspace persistence is implemented, including tab layouts and focused panes.
- SQLite-backed workspace storage is wired under `.deck/state.db`.
- Typed event bus is implemented.
- Environment capability detection is implemented.
- Pipe and PTY subprocess execution are implemented.
- Runtime task records now capture command name, exit state, timestamps, and output excerpts for startup probes.
- The shell can launch a recent command, rerun the latest task, cancel the active task, and stream live subprocess output into runtime task records.
- The command palette can run an arbitrary workspace command through `:run <command>` in addition to the saved recent-command shortcut.
- Recent-command task launches now use a PTY path so terminal-oriented tools keep TTY behavior while still feeding the Logs pane.
- The Run tab now includes a dedicated task-history pane, selected-task log inspection, and rerun of the focused recorded task.
- The Search pane now supports in-app `rg` queries with bounded async results and basic result selection.
- Selected search results can now open safely in `nvim` with line targeting through restored terminal I/O.
- The Files pane now supports basic directory navigation, file-open handoff into `nvim`, and a browsable current subtree.
- The fourth tab has been repurposed from research into a Finance workspace.
- Finance panes now render a watchlist, a focused symbol, and discovered local CSV/SQLite/JSON data sources instead of placeholder text.
- Finance watchlists can now be extended in-app and persisted to `.deck/watchlist.txt`.
- Direct API-backed quote refresh is now wired through Finnhub when `FINNHUB_API_KEY` and `curl` are available.
- Finance CSV ingestion now loads positions and balances from header-driven local `.csv` files into runtime state.
- The Portfolio pane now summarizes tracked positions, balances, cost basis, cash, market value, and daily change from available quotes.
- Market refresh now supports pluggable local/external sources by preferring local CSV quotes and falling back to Finnhub when available.
- Threshold alerts now load from `.deck/alerts.txt`, flag matching watchlist symbols, and surface triggered messages in the Finance panes and status line.
- Finance alerts can now also be added in-app from the Finance tab and persisted back to `.deck/alerts.txt`.
- The workspace summary now calls out safe-mode behavior and degraded capabilities per tab instead of leaving missing tools/providers implicit.
- `--safe` now provides a read-only, non-fullscreen workspace summary for recovery and diagnostics.
- The Portfolio pane now includes unrealized P/L, quoted-cost coverage, and simple best/worst daily mover summaries for quoted positions.
- The Portfolio pane now also calls out invested-vs-cash mix, quote coverage, largest holding concentration, and best/worst unrealized positions.
- Git status and diff pane redraws now reuse cached refresh-time command output instead of shelling out on every render.
- Moving between Review files now refreshes the selected diff asynchronously, so Git subprocess work does not block `j`/`k` navigation.
- A lightweight `:` command palette now routes common actions like run/rerun, search, git status/diff, refresh, and finance watchlist commands through the existing shell actions.
- Review tabs now keep a live parsed Git file list with selected-file diff rendering, stage/unstage shortcuts, a commit-message overlay, and palette equivalents for basic commit flow actions.
- Review tabs now support explicit file/hunk diff navigation through both keyboard shortcuts and palette commands.
- Review tabs now support selected-hunk stage/unstage actions against the diff currently in view.
- Review state now tracks local Git branches, with palette actions to switch branches or create-and-switch a local branch.
- Review state now also shows recent commits next to current branch and changed-file/hunk context.
- The Review tab can now toggle into its Files pane for normal `j`/`k` navigation and editor handoff, rather than leaving that pane display-only.
- Context-linked notes are now available for selected files, search results, and finance symbols, with note files persisted under `.deck/notes/`.
- A dedicated Notes tab now combines the context-note surface with a workspace scratchpad backed by `.deck/scratch.md`, both with inline editing, save, and reload controls.
- The shell footer now exposes status messaging for recent actions and degraded behavior.
- Pane rendering now uses cached snapshots instead of recomputing workspace scans and Git summaries on each redraw.
- The fullscreen shell now reserves its body for active panes, while Finance quote refreshes run off the UI thread and cached redraws no longer rescan the workspace for Logs.
- Task history is bounded and mouse-motion tracking is disabled so a long session or noisy terminal cannot create unnecessary redraw pressure.
- Existing persisted layouts are upgraded to include the Notes/Scratch tab when it is missing.
- `PaperAnchor` serialization and the legacy paper schema remain in persistence for now, but they are no longer part of the active MVP surface.
- Pane focus is now visible; Tab/Shift-Tab cycle panes, Ctrl-W toggles maximize, and Alt-arrows resize the nearest matching split.
- Ctrl-P provides a ranked mixed-source picker for actions, files, tabs, panes, recent commands, tasks, and safe project recipes.
- User task history now persists the newest 50 records with 4 KiB redacted output tails and session-only sensitive argv.
- A manual-refresh News tab provides cached AI, AI research, and Web3 security headlines from the public Hacker News Algolia endpoint.
- News Preview renders Hacker News text posts immediately and can fetch a bounded, session-cached readable excerpt for a selected public HTTPS article.

## Partially implemented

- Terminal and Logs panes reflect real runtime task records, including whether the latest task used a PTY or plain pipes.
- The Run tab now surfaces task history directly, but it is still a lightweight explorer rather than a broader process manager.
- Search pane can run and display `rg` queries and open selected results in `nvim`, but broader file navigation is still missing.
- File navigation is keyboard-driven and directory-aware, but it is still not a full tree widget.
- Git and Diff panes now support file-level staging, selected-hunk stage/unstage, selected-file diff inspection, commit submission, and local branch switching/creation, but not remote branch management.
- The typed command palette and fuzzy quick picker coexist; richer per-item previews remain future work.
- Finance panes now include quote refresh, CSV ingestion, portfolio summary metrics, allocation/concentration analytics, and threshold alerts.
- The shell has multi-tab structure and persistence, but tab-local interactivity remains thin.

## Known issues

- The UI can still feel laggy in broader cases, but Git-pane redraw hot paths are now cached instead of spawning Git each frame.

## Current MVP assessment

The shell, persistence model, and pane framework are now beyond scaffold level. The main missing work is interactivity: deeper dev workflow actions, lower-latency rendering behavior, and real finance data ingestion.
