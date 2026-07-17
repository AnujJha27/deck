#pragma once

#include "deck/types.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <string>
#include <vector>

namespace deck {

struct TaskRecord {
  std::string name;
  std::string command;
  std::vector<std::string> argv;
  bool use_pty = false;
  TaskState state = TaskState::Idle;
  int exit_code = 0;
  std::string started_at;
  std::string finished_at;
  std::string stdout_excerpt;
  std::string stderr_excerpt;
  bool timed_out = false;
  bool cancelled = false;
};

struct SearchResult {
  std::string path;
  int line = 0;
  int column = 0;
  std::string preview;
};

struct FileEntry {
  std::string path;
  bool is_directory = false;
};

struct PaperEntry {
  std::string path;
};

struct ImageCell {
  std::uint8_t upper_r = 0;
  std::uint8_t upper_g = 0;
  std::uint8_t upper_b = 0;
  std::uint8_t lower_r = 0;
  std::uint8_t lower_g = 0;
  std::uint8_t lower_b = 0;
  bool has_lower = false;
};

struct ImageRow {
  std::vector<ImageCell> cells;
};

struct MarketEntry {
  std::string symbol;
  std::string note;
};

struct MarketQuote {
  std::string symbol;
  bool has_data = false;
  double last_price = 0.0;
  double change = 0.0;
  double percent_change = 0.0;
  long long timestamp = 0;
  std::string status = "waiting";
  std::string provider = "none";
};

struct MarketCandle {
  std::string datetime;
  double open = 0.0;
  double high = 0.0;
  double low = 0.0;
  double close = 0.0;
  double volume = 0.0;
};

enum class NoteContextKind {
  None,
  File,
  SearchResult,
  Market,
};

struct NoteContext {
  NoteContextKind kind = NoteContextKind::None;
  std::string key;
  std::string label;
};

struct TextEditorState {
  std::string buffer;
  std::size_t cursor = 0;
  bool dirty = false;
  bool editing = false;
};

struct PositionEntry {
  std::string symbol;
  double quantity = 0.0;
  double cost_basis_total = 0.0;
  std::string source;
};

struct BalanceEntry {
  std::string label;
  double amount = 0.0;
  std::string currency = "USD";
  std::string source;
};

enum class AlertDirection {
  AboveOrEqual,
  BelowOrEqual,
};

struct AlertRule {
  std::string symbol;
  AlertDirection direction = AlertDirection::AboveOrEqual;
  double threshold = 0.0;
  std::string note;
  std::string source;
};

struct TriggeredAlert {
  std::string symbol;
  std::string message;
  std::string source;
};

struct GitStatusEntry {
  std::string path;
  std::string index_status = " ";
  std::string worktree_status = " ";
  bool renamed = false;
  std::string original_path;
};

struct GitBranchEntry {
  std::string name;
  bool current = false;
};

struct DiffHunk {
  int old_start = 0;
  int old_count = 0;
  int new_start = 0;
  int new_count = 0;
  std::string header;
};

struct TabPersistentState {
  std::string name;
  TabRole role = TabRole::Dev;
  SplitNode layout;
  PaneKind focused_pane = PaneKind::Files;
};

struct WorkspacePersistentState {
  std::string name;
  std::filesystem::path root;
  std::string external_editor = "nvim";
  std::vector<TabPersistentState> tabs;
  std::size_t focused_tab = 0;
  std::string selected_log_source = "task";
  std::vector<std::string> recent_commands;
  std::optional<PaperAnchor> last_anchor;
};

struct WorkspaceRuntimeState {
  TaskState active_task_state = TaskState::Idle;
  std::vector<TaskRecord> task_history;
  std::size_t selected_task_index = 0;
  std::string files_browser_root = ".";
  std::vector<FileEntry> files_entries;
  std::size_t selected_file_index = 0;
  std::string current_search_query;
  std::vector<SearchResult> search_results;
  std::size_t selected_search_result = 0;
  bool search_in_progress = false;
  std::size_t search_generation = 0;
  std::vector<GitStatusEntry> git_entries;
  std::size_t selected_git_index = 0;
  bool review_files_mode = false;
  std::vector<GitBranchEntry> git_branches;
  std::string current_git_branch;
  std::vector<std::string> git_recent_commits;
  std::vector<DiffHunk> diff_hunks;
  std::size_t selected_diff_hunk = 0;
  std::string git_status_text;
  std::string diff_preview_text;
  std::vector<MarketEntry> market_entries;
  std::size_t selected_market_index = 0;
  std::string current_market_symbol;
  std::unordered_map<std::string, MarketQuote> market_quotes;
  std::unordered_map<std::string, std::vector<MarketCandle>> market_candles;
  std::vector<std::string> finance_data_sources;
  std::vector<PositionEntry> positions;
  std::vector<BalanceEntry> balances;
  std::vector<AlertRule> alert_rules;
  std::vector<TriggeredAlert> triggered_alerts;
  std::vector<std::string> portfolio_lines;
  NoteContext note_context;
  TextEditorState note_editor;
  TextEditorState scratch_editor;
  std::string market_data_provider = "none";
  bool market_data_enabled = false;
  bool market_data_refresh_in_progress = false;
  std::vector<std::string> palette_suggestions;
  std::string status_message;
  std::size_t next_generation = 1;
  bool overlays_enabled = true;
  std::size_t visible_tab = 0;
  std::optional<PaneKind> maximized_pane;
  std::string math_expression;
  std::string math_result;
  std::string math_plot;
  std::vector<std::pair<std::string, std::string>> math_history;
  double math_plot_min = -10.0;
  double math_plot_max = 10.0;
};

WorkspacePersistentState make_default_workspace(const std::filesystem::path& root);
void ensure_workspace_tabs(WorkspacePersistentState& state);
std::string serialize_workspace(const WorkspacePersistentState& state);
std::optional<WorkspacePersistentState> parse_workspace(
    const std::string& text, const std::filesystem::path& fallback_root);

std::filesystem::path state_file_for(const std::filesystem::path& root);
std::filesystem::path config_dir_for(const std::filesystem::path& root);
std::filesystem::path database_file_for(const std::filesystem::path& root);

}  // namespace deck
