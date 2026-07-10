# Roadmap

Last updated: 2026-07-10

## Core checklist

- [x] Replace placeholder pane text with real workspace summaries.
- [x] Preserve a stable build and test baseline while the shell evolves.
- [x] Pivot the fourth tab away from research into a finance workspace.

## Runtime and tasks

- [x] Add a real task model that records launches, output, exit status, and timestamps.
- [x] Wire the Run/Logs panes to live subprocess output.
- [x] Add rerun/cancel semantics for the single active workspace task.
- [x] Introduce refresh hooks so panes can update without restarting the UI.
- [x] Add a lightweight command palette for common actions.

## Dev workflow

- [x] Add interactive file navigation.
- [x] Add safe file-open actions into `nvim`.
- [x] Add actual `rg` query execution and bounded result navigation.
- [x] Add actionable git refresh, stage, unstage, and commit flows.
- [ ] Add diff navigation between changed files/hunks.
- [x] Add context-linked notes for files, search results, and finance symbols.
- [x] Add a dedicated workspace scratchspace pane with inline editing.

## Finance workspace

- [x] Replace paper panes with Markets and Portfolio panes.
- [x] Discover a watchlist from `.deck/watchlist.txt` with a sane fallback list.
- [x] Discover local finance data sources from CSV/SQLite/JSON files.
- [x] Allow keyboard focus changes across watchlist entries.
- [x] Add in-app ticker input and persist watchlist updates to `.deck/watchlist.txt`.
- [x] Add direct API-backed quote refresh using a configured provider key.
- [x] Ingest positions and balances from CSV.
- [x] Add a portfolio summary with totals, cash, and daily change.
- [x] Add watchlist quote refresh from pluggable local/external sources beyond the first provider.
- [x] Add alerts and threshold tracking.

## Shell polish

- [x] Add pane-level refresh and status messaging.
- [ ] Add better safe-mode and degraded-capability explanations in the UI.
- [ ] Fix terminal shutdown so quitting never leaves mouse/escape garbage printed.
- [ ] Reduce tab-switch/render lag under repeated redraws.

## Exit criteria for MVP

- Open a workspace and get meaningful pane content immediately.
- Run a command and inspect its output from inside the shell.
- Review workspace git state without leaving the app.
- Use the Finance tab to inspect a watchlist and discovered local finance data sources.
