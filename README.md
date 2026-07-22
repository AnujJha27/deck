# deck

`deck` is a terminal workspace for development and market monitoring. It combines a multi-pane shell, Git review tools, workspace notes, and a finance dashboard inside a single fullscreen TUI built in C++ with FTXUI.

## What it does

- run and rerun workspace commands
- stream command output from pipe or PTY-backed tasks
- inspect recent task output
- search the workspace with `rg`
- review Git status and diffs from inside the shell
- navigate files and open them in Vim, Neovim, or VS Code
- manage a watchlist with local CSV, Twelve Data candle history, or Finnhub quote fallback
- ingest portfolio positions and balances from CSV
- track threshold alerts
- keep context-linked notes and a general scratchpad

## Current features

### Workspace shell

- fullscreen FTXUI interface with persistent tabs and split panes
- compact tab/footer chrome that leaves the full body for active panes
- command palette for common actions
- PTY-backed recent-command execution with rerun/cancel support
- dedicated Run-tab task history explorer with selected-task logs, rerun, and cancellation
- status footer and degraded-capability messaging
- safe mode for reduced terminal interaction

### Development workflow

- directory-aware file list with keyboard navigation
- `rg`-based search with in-app query entry
- safe editor handoff into a selected Vim, Neovim, or VS Code
- Git status, diff, stage, unstage, and commit actions
- current branch and recent commit history alongside file/hunk review
- inspect local branches and switch to or create branches from the command palette
- diff navigation across files and hunks
- selected-hunk stage and unstage actions from the Review tab

### Finance workspace

- watchlist discovery from `.deck/watchlist.txt`
- candle/quote refresh from local OHLC CSV data, Twelve Data, or optional Finnhub fallback
- threshold alerts loaded from or written to `.deck/alerts.txt`
- compact daily candlestick chart for the focused watchlist symbol

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

For daily OHLC candles, Twelve Data is preferred:

```bash
export TWELVE_DATA_API_KEY=your_key_here
```

Local OHLC CSV files work without an API key when they contain:

```text
symbol,date,open,high,low,close,volume
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

- `Tab` / `Shift+Tab` cycle panes in layout order
- `Ctrl+W` toggle the focused pane between maximized and tiled
- `Alt+Arrow` move the nearest matching split by 5%
- `Ctrl+P` open the fuzzy quick picker for files, actions, tabs, panes, tasks, commands, and project recipes
- `:` open command palette
- `:run <command>` run any workspace command (for example, `:run cmake --build build`)
- `:editor vim`, `:editor nvim`, or `:editor vscode` choose the editor used when opening files (saved per workspace)
- `,` open Settings to choose an editor and see whether it is available on `PATH`
- `c` inside Settings clears persisted task history
- `q` quit
- `r` run recent command
- `R` rerun latest task
- `x` refresh current surface or cancel active task
- `:clear-tasks` clear persisted task history

Dev is for launching a command beside files and search. Run is for inspecting its recorded output, choosing an older task, rerunning it, or cancelling the active task.

Run controls:

- `j` and `k` move through recorded tasks in the Run tab
- `Enter` reruns the selected task from the Run tab

Review controls:

- `s` stage selected file
- `u` unstage selected file
- `S` stage selected hunk from the current unstaged diff view
- `U` unstage selected hunk from the current staged diff view
- changed-file rows show their available `[s stage]` / `[u unstage]` actions
- `[` and `]` move between hunks
- `f` toggle the Review tab between changed-file review and the left Files pane
- `:branch <name>` switch to an existing local branch
- `:branch-new <name>` create and switch to a new local branch

PTY-backed command tasks stream through the Logs pane as a single terminal-style output channel, so interactive programs keep their expected TTY behavior.

Finance controls:

- `a` add ticker to watchlist
- `d` remove the selected ticker from the watchlist
- `A` add alert rule
- `e` edit note for selected symbol

The Finance candle pane uses a responsive two-column layout: candles expand within the left column, while the right column combines a focused market snapshot with a relative-volume chart.

Notes controls:

- `E` edit current context note
- `Ctrl+S` save
- `Ctrl+R` reload
- `Esc` stop editing
- `PageUp` and `PageDown` scroll the focused pane; use `Tab` to move pane focus

The Notes tab contains the workspace Scratch pane. Existing saved workspaces are upgraded to include it automatically on the next launch.

News controls:

- `x` manually fetch recent headlines
- `[` and `]` switch between AI World, AI Research, and Web3 Security
- `j` and `k` select a headline
- `v` fetch a bounded readable-text preview for the selected external article
- `PageUp` and `PageDown` scroll the focused pane (`v` focuses the article preview)
- `Enter` open the selected story through `wslview`, `xdg-open`, macOS `open`, or WSL's Windows browser bridge

Hacker News text posts appear in Preview immediately. External article text is fetched only when you press `v`, is limited to public HTTPS URLs and 256 KiB of HTML, and is reduced to a short session-cached reader view that preserves headings, paragraphs, and lists while dropping common page chrome. News uses the public Hacker News Algolia search endpoint without an API key or paid plan. Results stay cached in memory until you manually refresh; Deck does not poll in the background. Existing workspaces gain the News tab automatically.

The Files pane can scroll through selected text files up to 512 KiB, including larger source files. Binary files and files beyond that safety limit are identified without loading their contents.

When `pygmentize` from Pygments is installed, File Preview uses its language-aware token stream for syntax colors in C++, Python, JavaScript/TypeScript, JSON, shell, CMake, and Markdown. It remains a plain readable preview when Pygments is unavailable.

## Workspace files

`deck` stores local workspace state under `.deck/`:

- `.deck/state.db`: SQLite-backed state
- `.deck/watchlist.txt`: tracked symbols
- `.deck/alerts.txt`: threshold rules
- `.deck/scratch.md`: scratchpad
- `.deck/notes/`: context-linked notes

The SQLite state also retains the newest 50 user task records with redacted 4 KiB output tails. Probe tasks are not persisted, and commands whose argv appears sensitive remain session-only.

## Project docs

- `IMPLEMENTATION_STATUS.md` for current status
- `ROADMAP.md` for planned work
- `ARCHITECTURE.md` for structure and design notes
