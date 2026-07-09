# Vision

Last updated: 2026-07-10

## What `deck` is

`deck` is a terminal-native workstation for software engineering and personal market monitoring. It is meant to be a place you live inside, not a demo shell and not a clone of a single existing tool.

The target feel is:

- persistent
- composable
- fast to reopen
- event-driven across panes
- strong enough for daily project work

## What makes it different

The differentiator is not "many modules". The differentiator is one shared workspace runtime where panes cooperate:

- files retarget git and terminal context
- task output flows into logs and review surfaces
- search results hand off directly into editing
- notes follow the current code/search/market context while scratchspace stays available for general capture
- market context lives beside the dev workflow instead of in a separate app
- workspace state survives restarts cleanly

## Product priorities

1. Software engineering workflow depth
2. Personal finance/watchlist workflow utility
3. Clean recovery and persistence behavior
4. Strong terminal UX through `FTXUI`

## Non-goals for the current reboot

- building a full OS-like environment
- competing with full GUI IDEs on code editing
- shipping every planned module at once
- forcing rich PDF reading into a terminal-first product
