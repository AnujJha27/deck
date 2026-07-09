# Decisions

Last updated: 2026-07-10

## Confirmed decisions

### C++ + `FTXUI`

The reboot is intentionally C++, and `FTXUI` is the shell/UI framework. This is not a Rust rewrite.

### Ambitious MVP, but scoped

The MVP should feel substantial through workflow depth and persistence, not by attempting every possible module.

### One shared process path

External tool execution should go through a single subprocess abstraction instead of each pane improvising its own command spawning.

### One active task model per workspace

The near-term target is one meaningful task surface per workspace, not several interactive PTYs.

### Pivot away from in-app paper reading

The paper-reader direction is no longer part of the active MVP. On a Windows-first personal setup, comfortable PDF/image reading is the wrong fit for this terminal surface. The fourth tab now targets finance/watchlist workflow instead.

### Preserve legacy paper persistence for now

Existing `PaperAnchor` and paper-schema code can remain temporarily as backend residue while the UI and roadmap pivot. Deletion is a follow-up cleanup, not part of the immediate product correction.

### Separate linked notes from scratchspace

The first writing surfaces should be split cleanly:

- linked notes attach to current file/search/finance context
- scratchspace is one general workspace-local document
- both use inline editing primitives, but they are not the same product surface

## Open decisions

- whether the first interactive task implementation is true PTY or a stronger non-PTY bridge
- when to add pane-local interactivity versus keep refining runtime summaries
- whether finance quote refresh should start as file-backed only or support external providers early
