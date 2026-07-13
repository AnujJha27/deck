# deck

`deck` is a terminal workspace for development and market monitoring. It combines a multi-pane shell, Git review tools, workspace notes, and a finance dashboard inside a single fullscreen TUI built in C++ with FTXUI.

## What it does

- run and rerun workspace commands
- stream command output from pipe or PTY-backed tasks
- inspect recent task output
- search the workspace with `rg`
- review Git status and diffs from inside the shell
- navigate files and open them in `nvim`
- manage a watchlist with local or Finnhub-backed quotes
- ingest portfolio positions and balances from CSV
- track threshold alerts
- keep context-linked notes and a general scratchpad

## Current features

### Workspace shell

- fullscreen FTXUI interface with persistent tabs and split panes
- compact tab/footer chrome that leaves the full body for active panes
- command palette for common actions
- PTY-backed recent-command execution with rerun/cancel support
- dedicated Run-tab task history explorer with selected-task logs
- status footer and degraded-capability messaging
- safe mode for reduced terminal interaction

### Development workflow

- directory-aware file list with keyboard navigation
- `rg`-based search with in-app query entry
- safe editor handoff into `nvim`
- Git status, diff, stage, unstage, and commit actions
- inspect local branches and switch to or create branches from the command palette
- diff navigation across files and hunks
- selected-hunk stage and unstage actions from the Review tab

### Finance workspace

- watchlist discovery from `.deck/watchlist.txt`
- quote refresh from local CSV data and optional Finnhub fallback
- CSV ingestion for positions and balances
- portfolio totals, cash, daily change, unrealized P/L, allocation mix, concentration, and basic movers
- threshold alerts loaded from or written to `.deck/alerts.txt`

### Notes

- context-linked notes for selected files, search results, and market symbols
- workspace scratchpad stored in `.deck/scratch.md`

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

## Run

```bash
./build/deck
```

Useful variants:

```bash
./build/deck --safe
./build/deck doctor
./build/deck workspace list
./build/deck workspace open .
```

`--safe` prints a read-only workspace and capability summary without entering fullscreen mode. It is useful when terminal rendering or persisted UI state is suspect.

## Finance setup

For live API-backed quotes, set:

```bash
export FINNHUB_API_KEY=your_key_here
```

or place it in `.env`:

```bash
FINNHUB_API_KEY=your_key_here
```

`deck doctor` reports whether the key was found and where it was discovered from, which is the quickest way to verify `.env` detection.

Local quote CSVs can also provide prices without an API key. Example headers:

```text
symbol,price,change,percent_change
```

Position CSVs can use headers such as:

```text
symbol,quantity,cost_basis_total
```

Balance CSVs can use headers such as:

```text
account,amount,currency
```

## In-app controls

Common controls:

- `:` open command palette
- `:run <command>` run any workspace command (for example, `:run cmake --build build`)
- `q` quit
- `r` run recent command
- `R` rerun latest task
- `x` refresh current surface or cancel active task

Run controls:

- `j` and `k` move through recorded tasks in the Run tab
- `Enter` reruns the selected task from the Run tab

Review controls:

- `s` stage selected file
- `u` unstage selected file
- `S` stage selected hunk from the current unstaged diff view
- `U` unstage selected hunk from the current staged diff view
- `[` and `]` move between hunks
- `:branch <name>` switch to an existing local branch
- `:branch-new <name>` create and switch to a new local branch

PTY-backed command tasks stream through the Logs pane as a single terminal-style output channel, so interactive programs keep their expected TTY behavior.

Finance controls:

- `a` add ticker to watchlist
- `A` add alert rule
- `e` edit note for selected symbol

Notes controls:

- `E` edit current context note
- `Ctrl+S` save
- `Ctrl+R` reload
- `Esc` stop editing

The Notes tab contains the workspace Scratch pane. Existing saved workspaces are upgraded to include it automatically on the next launch.

## Workspace files

`deck` stores local workspace state under `.deck/`:

- `.deck/state.db`: SQLite-backed state
- `.deck/watchlist.txt`: tracked symbols
- `.deck/alerts.txt`: threshold rules
- `.deck/scratch.md`: scratchpad
- `.deck/notes/`: context-linked notes

## Project docs

- `IMPLEMENTATION_STATUS.md` for current status
- `ROADMAP.md` for planned work
- `ARCHITECTURE.md` for structure and design notes
