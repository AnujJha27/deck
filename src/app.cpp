#include "deck/app.h"

#include "deck/environment.h"
#include "deck/event_bus.h"
#include "deck/panes.h"
#include "deck/persistence.h"
#include "deck/picker.h"
#include "deck/process.h"
#include "deck/split_tree.h"
#include "deck/workspace.h"

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <cstdlib>
#include <thread>

namespace deck {
namespace {
using namespace ftxui;

std::string wrap_text(const std::string& text, std::size_t width = 64) {
  std::istringstream in(text);
  std::ostringstream out;
  std::string word;
  std::size_t line = 0;
  while (in >> word) {
    if (line != 0 && line + word.size() + 1 > width) {
      out << "\n";
      line = 0;
    }
    if (line != 0) {
      out << " ";
      ++line;
    }
    out << word;
    line += word.size();
  }
  return out.str();
}

std::string format_timestamp(std::chrono::system_clock::time_point time_point) {
  const std::time_t raw = std::chrono::system_clock::to_time_t(time_point);
  std::tm local_tm{};
#if defined(_WIN32)
  localtime_s(&local_tm, &raw);
#else
  localtime_r(&raw, &local_tm);
#endif
  std::ostringstream out;
  out << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S");
  return out.str();
}

std::string join_argv(const std::vector<std::string>& argv) {
  std::ostringstream out;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    if (i != 0) {
      out << ' ';
    }
    out << argv[i];
  }
  return out.str();
}

std::string clip_text(std::string text, std::size_t limit = 240) {
  if (text.size() <= limit) {
    return text;
  }
  return text.substr(0, limit) + "...";
}

std::filesystem::path scratch_file_path(const std::filesystem::path& root) {
  return config_dir_for(root) / "scratch.md";
}

std::string note_context_directory(NoteContextKind kind) {
  switch (kind) {
    case NoteContextKind::File:
      return "files";
    case NoteContextKind::SearchResult:
      return "search";
    case NoteContextKind::Market:
      return "markets";
    case NoteContextKind::None:
      break;
  }
  return "general";
}

std::string sanitize_note_key(const std::string& key) {
  std::string result;
  result.reserve(key.size());
  for (unsigned char ch : key) {
    if (std::isalnum(ch)) {
      result.push_back(static_cast<char>(std::tolower(ch)));
    } else if (ch == '-' || ch == '_') {
      result.push_back(static_cast<char>(ch));
    } else {
      result.push_back('_');
    }
  }
  while (!result.empty() && result.back() == '_') {
    result.pop_back();
  }
  if (result.empty()) {
    result = "note";
  }
  if (result.size() > 48) {
    result.resize(48);
  }
  return result;
}

std::filesystem::path note_file_path(const std::filesystem::path& root, const NoteContext& context) {
  const auto hash_value = static_cast<unsigned long long>(std::hash<std::string>{}(context.key));
  std::ostringstream filename;
  filename << sanitize_note_key(context.label.empty() ? context.key : context.label) << "-" << std::hex << hash_value
           << ".md";
  return config_dir_for(root) / "notes" / note_context_directory(context.kind) / filename.str();
}

std::string load_context_note(const std::filesystem::path& root, const NoteContext& context) {
  if (context.kind == NoteContextKind::None || context.key.empty()) {
    return {};
  }
  std::ifstream in(note_file_path(root, context));
  if (!in) {
    return {};
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool save_context_note(const std::filesystem::path& root, const NoteContext& context, const std::string& text) {
  if (context.kind == NoteContextKind::None || context.key.empty()) {
    return false;
  }
  const auto path = note_file_path(root, context);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return false;
  }
  out << text;
  return static_cast<bool>(out);
}

std::string load_scratch_buffer(const std::filesystem::path& root) {
  std::ifstream in(scratch_file_path(root));
  if (!in) {
    return {};
  }
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool save_scratch_buffer(const std::filesystem::path& root, const std::string& text) {
  std::error_code ec;
  std::filesystem::create_directories(config_dir_for(root), ec);
  std::ofstream out(scratch_file_path(root), std::ios::trunc);
  if (!out) {
    return false;
  }
  out << text;
  return static_cast<bool>(out);
}

NoteContext market_note_context(const WorkspaceRuntimeState& runtime) {
  if (runtime.market_entries.empty() || runtime.selected_market_index >= runtime.market_entries.size()) {
    return {};
  }
  const auto& entry = runtime.market_entries[runtime.selected_market_index];
  return {NoteContextKind::Market, entry.symbol, "symbol-" + entry.symbol};
}

NoteContext file_note_context(const WorkspaceRuntimeState& runtime) {
  if (runtime.files_entries.empty() || runtime.selected_file_index >= runtime.files_entries.size()) {
    return {};
  }
  const auto& entry = runtime.files_entries[runtime.selected_file_index];
  if (entry.is_directory) {
    return {};
  }
  const auto full_path = (std::filesystem::path(runtime.files_browser_root) / entry.path).lexically_normal().string();
  return {NoteContextKind::File, full_path, full_path};
}

NoteContext search_note_context(const WorkspaceRuntimeState& runtime) {
  if (runtime.search_results.empty() || runtime.selected_search_result >= runtime.search_results.size()) {
    return {};
  }
  const auto& result = runtime.search_results[runtime.selected_search_result];
  return {NoteContextKind::SearchResult,
          result.path + ":" + std::to_string(result.line) + ":" + std::to_string(result.column),
          result.path + "-" + std::to_string(result.line)};
}

NoteContext desired_note_context(const WorkspaceRuntimeState& runtime) {
  if (!runtime.search_results.empty()) {
    return search_note_context(runtime);
  }
  if (!runtime.files_entries.empty()) {
    return file_note_context(runtime);
  }
  if (!runtime.market_entries.empty()) {
    return market_note_context(runtime);
  }
  return {};
}

bool switch_note_context(const std::filesystem::path& root,
                         WorkspaceRuntimeState& runtime,
                         const NoteContext& context) {
  if (context.kind == NoteContextKind::None || context.key.empty()) {
    return false;
  }
  if (runtime.note_editor.editing || runtime.note_editor.dirty) {
    return false;
  }
  if (runtime.note_context.kind == context.kind && runtime.note_context.key == context.key) {
    return false;
  }
  runtime.note_context = context;
  runtime.note_editor.buffer = load_context_note(root, context);
  runtime.note_editor.cursor = runtime.note_editor.buffer.size();
  runtime.note_editor.dirty = false;
  runtime.note_editor.editing = false;
  return true;
}

void refresh_note_context(const std::filesystem::path& root, WorkspaceRuntimeState& runtime) {
  switch_note_context(root, runtime, desired_note_context(runtime));
}

std::size_t line_start_for(const std::string& text, std::size_t cursor) {
  cursor = std::min(cursor, text.size());
  while (cursor > 0 && text[cursor - 1] != '\n') {
    --cursor;
  }
  return cursor;
}

std::size_t line_end_for(const std::string& text, std::size_t cursor) {
  cursor = std::min(cursor, text.size());
  while (cursor < text.size() && text[cursor] != '\n') {
    ++cursor;
  }
  return cursor;
}

std::size_t move_cursor_vertical(const std::string& text, std::size_t cursor, int direction) {
  const auto current_start = line_start_for(text, cursor);
  const auto current_end = line_end_for(text, cursor);
  const auto column = cursor - current_start;

  if (direction < 0) {
    if (current_start == 0) {
      return cursor;
    }
    const auto previous_end = current_start - 1;
    const auto previous_start = line_start_for(text, previous_end);
    return std::min(previous_start + column, previous_end);
  }

  if (current_end >= text.size()) {
    return cursor;
  }
  const auto next_start = current_end + 1;
  const auto next_end = line_end_for(text, next_start);
  return std::min(next_start + column, next_end);
}

std::string editor_display_name(const std::string& editor) {
  return editor == "vscode" ? "VS Code" : editor == "vim" ? "Vim" : "Neovim";
}

bool editor_available(const std::string& editor, const EnvironmentCapabilities& caps) {
  return editor == "vscode" ? caps.vscode : editor == "vim" ? caps.vim : caps.nvim;
}

bool open_in_configured_editor(ScreenInteractive& screen,
                               const WorkspacePersistentState& state,
                               const EnvironmentCapabilities& caps,
                               const SearchResult& result) {
  if (!editor_available(state.external_editor, caps)) {
    return false;
  }
  ProcessRunner runner;
  ProcessRequest request;
  request.cwd = state.root;
  const auto full_path = std::filesystem::absolute(state.root / result.path);
  const auto line = std::max(result.line, 1);
  if (state.external_editor == "vscode") {
    request.argv = {"code", "--goto", full_path.string() + ":" + std::to_string(line) + ":" +
                                           std::to_string(std::max(result.column, 1))};
  } else {
    const auto editor = state.external_editor == "vim" ? "vim" : "nvim";
    request.argv = {editor, "+" + std::to_string(line), full_path.string()};
  }
  auto attached = screen.WithRestoredIO([&] { runner.run_attached(request); });
  attached();
  return true;
}

void append_tail(std::string& target, const std::string& chunk, std::size_t limit = 4096) {
  target.append(chunk);
  if (target.size() > limit) {
    target.erase(0, target.size() - limit);
  }
}

std::vector<std::string> split_command_line(const std::string& command) {
  std::vector<std::string> argv;
  std::string current;
  bool in_single = false;
  bool in_double = false;
  bool escaping = false;

  for (char ch : command) {
    if (escaping) {
      current.push_back(ch);
      escaping = false;
      continue;
    }
    if (ch == '\\') {
      escaping = true;
      continue;
    }
    if (ch == '\'' && !in_double) {
      in_single = !in_single;
      continue;
    }
    if (ch == '"' && !in_single) {
      in_double = !in_double;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(ch)) && !in_single && !in_double) {
      if (!current.empty()) {
        argv.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) {
    argv.push_back(current);
  }
  return argv;
}

TaskRecord make_task_record(const std::string& name, const std::vector<std::string>& argv, bool use_pty = false) {
  TaskRecord record;
  record.name = name;
  record.argv = argv;
  record.use_pty = use_pty;
  record.command = join_argv(argv);
  record.state = TaskState::Starting;
  record.started_at = format_timestamp(std::chrono::system_clock::now());
  return record;
}

TaskRecord execute_task_probe(const std::string& name,
                              const ProcessRequest& request,
                              const ProcessRunner& runner) {
  auto record = make_task_record(name, request.argv);

  const auto result = runner.run(request);

  record.finished_at = format_timestamp(std::chrono::system_clock::now());
  record.exit_code = result.exit_code;
  record.stdout_excerpt = clip_text(result.stdout_text);
  record.stderr_excerpt = clip_text(result.stderr_text);
  record.timed_out = result.timed_out;
  record.cancelled = result.cancelled;

  if (result.cancelled) {
    record.state = TaskState::Cancelled;
  } else if (result.timed_out || result.exit_code != 0) {
    record.state = TaskState::Failed;
  } else {
    record.state = TaskState::Exited;
  }
  return record;
}

struct ShellTaskController {
  std::mutex mutex;
  WorkspaceRuntimeState runtime;
  std::optional<std::size_t> active_task_index;
  std::atomic<bool> cancel_requested = false;
  std::atomic<bool> task_running = false;
  std::atomic<bool> exit_requested = false;
  std::jthread worker;
  std::jthread search_worker;
  std::jthread market_worker;
  std::jthread git_diff_worker;
  std::atomic<bool> git_diff_worker_running = false;
  std::atomic<std::size_t> git_diff_generation = 0;
};

struct SearchOverlayState {
  bool active = false;
  std::string query;
};

struct TickerOverlayState {
  bool active = false;
  std::string symbol;
};

struct AlertOverlayState {
  bool active = false;
  std::string rule;
};

struct CommandOverlayState {
  bool active = false;
  std::string command;
};

struct CommitOverlayState {
  bool active = false;
  std::string message;
};

struct SettingsOverlayState {
  bool active = false;
  int selected_editor = 1;
};

enum class PickerEntryKind { Action, File, Tab, Pane, RecentCommand, Task, Recipe };

struct PickerEntry {
  PickerEntryKind kind = PickerEntryKind::Action;
  std::string label;
  std::vector<std::string> argv;
  std::size_t tab_index = 0;
  std::optional<PaneKind> pane;
  std::string value;
};

struct PickerOverlayState {
  bool active = false;
  std::string query;
  std::size_t selected = 0;
};

void set_status(ShellTaskController& controller, const std::string& message);

std::vector<FileEntry> discover_file_entries(const std::filesystem::path& root,
                                             const std::filesystem::path& relative_root = ".") {
  std::vector<FileEntry> entries;
  const auto browse_root = std::filesystem::weakly_canonical(root / relative_root);
  std::error_code ec;
  const auto canonical_root = std::filesystem::weakly_canonical(root, ec);
  ec.clear();
  if (browse_root != canonical_root) {
    entries.push_back(FileEntry{"..", true});
  }

  if (!std::filesystem::exists(browse_root, ec) || !std::filesystem::is_directory(browse_root, ec)) {
    return entries;
  }

  std::vector<FileEntry> directories;
  std::vector<FileEntry> files;
  for (const auto& entry : std::filesystem::directory_iterator(
           browse_root, std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (ec) {
      break;
    }
    const auto name = entry.path().filename().string();
    if (name == ".git" || name == ".deck" || name == "build") {
      continue;
    }
    auto relative = std::filesystem::relative(entry.path(), browse_root, ec);
    if (ec) {
      ec.clear();
      continue;
    }
    if (entry.is_directory(ec)) {
      directories.push_back(FileEntry{relative.string(), true});
    } else if (entry.is_regular_file(ec)) {
      files.push_back(FileEntry{relative.string(), false});
    }
    ec.clear();
  }

  const auto sorter = [](const auto& lhs, const auto& rhs) { return lhs.path < rhs.path; };
  std::sort(directories.begin(), directories.end(), sorter);
  std::sort(files.begin(), files.end(), sorter);
  entries.insert(entries.end(), directories.begin(), directories.end());
  entries.insert(entries.end(), files.begin(), files.end());
  if (entries.size() > 64) {
    entries.resize(64);
  }
  return entries;
}

std::vector<MarketEntry> default_watchlist() {
  return {
      {"SPY", "S&P 500"},
      {"QQQ", "Nasdaq 100"},
      {"NVDA", "AI semis"},
      {"MSFT", "Platform"},
      {"AAPL", "Hardware"},
      {"AMZN", "Consumer/cloud"},
  };
}

std::vector<MarketEntry> discover_market_entries(const std::filesystem::path& root) {
  const auto watchlist_path = root / ".deck" / "watchlist.txt";
  std::vector<MarketEntry> entries;
  std::ifstream in(watchlist_path);
  std::string line;
  while (std::getline(in, line)) {
    auto cleaned = clip_text(line, 120);
    cleaned.erase(std::remove(cleaned.begin(), cleaned.end(), '\r'), cleaned.end());
    if (cleaned.empty() || cleaned[0] == '#') {
      continue;
    }
    auto split = cleaned.find_first_of(" \t:");
    if (split == std::string::npos) {
      entries.push_back({cleaned, "watchlist"});
      continue;
    }
    auto symbol = cleaned.substr(0, split);
    auto note = cleaned.substr(split + 1);
    note.erase(0, note.find_first_not_of(" \t:"));
    entries.push_back({symbol, note.empty() ? std::string("watchlist") : note});
  }
  if (entries.empty()) {
    entries = default_watchlist();
  }
  if (entries.size() > 32) {
    entries.resize(32);
  }
  return entries;
}

bool persist_watchlist(const std::filesystem::path& root, const std::vector<MarketEntry>& entries) {
  std::error_code ec;
  std::filesystem::create_directories(root / ".deck", ec);
  std::ofstream out(root / ".deck" / "watchlist.txt", std::ios::trunc);
  if (!out) {
    return false;
  }
  for (const auto& entry : entries) {
    out << entry.symbol;
    if (!entry.note.empty()) {
      out << " " << entry.note;
    }
    out << "\n";
  }
  return true;
}

std::vector<std::string> discover_finance_sources(const std::filesystem::path& root) {
  std::vector<std::string> sources;
  for (const auto& file : discover_file_entries(root)) {
    const auto extension = std::filesystem::path(file.path).extension().string();
    if (extension == ".csv" || extension == ".db" || extension == ".sqlite" || extension == ".sqlite3" ||
        extension == ".json") {
      sources.push_back(file.path);
    }
  }
  if (sources.size() > 12) {
    sources.resize(12);
  }
  return sources;
}

std::string lower_copy(std::string value) {
  std::transform(value.begin(),
                 value.end(),
                 value.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

std::string trim_copy(std::string value) {
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

std::vector<std::string> split_csv_row(const std::string& row) {
  std::vector<std::string> cells;
  std::string current;
  bool in_quotes = false;
  const char delimiter = row.find(',') != std::string::npos ? ',' : ';';
  for (std::size_t i = 0; i < row.size(); ++i) {
    const char ch = row[i];
    if (ch == '"') {
      if (in_quotes && i + 1 < row.size() && row[i + 1] == '"') {
        current.push_back('"');
        ++i;
      } else {
        in_quotes = !in_quotes;
      }
      continue;
    }
    if (ch == delimiter && !in_quotes) {
      cells.push_back(trim_copy(current));
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  cells.push_back(trim_copy(current));
  return cells;
}

std::optional<double> parse_decimal(const std::string& raw) {
  auto cleaned = trim_copy(raw);
  cleaned.erase(std::remove(cleaned.begin(), cleaned.end(), '$'), cleaned.end());
  cleaned.erase(std::remove(cleaned.begin(), cleaned.end(), ','), cleaned.end());
  cleaned.erase(std::remove(cleaned.begin(), cleaned.end(), '%'), cleaned.end());
  if (cleaned.empty()) {
    return std::nullopt;
  }
  char* end = nullptr;
  const auto value = std::strtod(cleaned.c_str(), &end);
  if (end == cleaned.c_str() || (end != nullptr && *end != '\0')) {
    return std::nullopt;
  }
  return value;
}

int header_index(const std::vector<std::string>& headers, std::initializer_list<std::string_view> names) {
  for (std::size_t i = 0; i < headers.size(); ++i) {
    for (const auto name : names) {
      if (headers[i] == name) {
        return static_cast<int>(i);
      }
    }
  }
  return -1;
}

std::vector<PositionEntry> load_positions_from_csv(const std::filesystem::path& root,
                                                   const std::vector<std::string>& sources) {
  std::vector<PositionEntry> raw_positions;
  for (const auto& source : sources) {
    if (std::filesystem::path(source).extension() != ".csv") {
      continue;
    }
    std::ifstream in(root / source);
    if (!in) {
      continue;
    }
    std::string header_line;
    if (!std::getline(in, header_line)) {
      continue;
    }
    auto headers = split_csv_row(header_line);
    for (auto& header : headers) {
      header = lower_copy(header);
    }
    const int symbol_idx = header_index(headers, {"symbol", "ticker", "asset"});
    const int quantity_idx = header_index(headers, {"quantity", "qty", "shares", "units"});
    if (symbol_idx < 0 || quantity_idx < 0) {
      continue;
    }
    const int total_cost_idx = header_index(headers, {"cost_basis_total", "total_cost", "book_value", "cost_basis"});
    const int avg_cost_idx = header_index(headers, {"average_cost", "avg_cost", "cost_per_share", "price"});
    std::string row;
    while (std::getline(in, row)) {
      auto cells = split_csv_row(row);
      if (symbol_idx >= static_cast<int>(cells.size()) || quantity_idx >= static_cast<int>(cells.size())) {
        continue;
      }
      auto symbol = trim_copy(cells[static_cast<std::size_t>(symbol_idx)]);
      std::transform(symbol.begin(),
                     symbol.end(),
                     symbol.begin(),
                     [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
      auto quantity = parse_decimal(cells[static_cast<std::size_t>(quantity_idx)]);
      if (symbol.empty() || !quantity) {
        continue;
      }
      double cost_basis_total = 0.0;
      if (total_cost_idx >= 0 && total_cost_idx < static_cast<int>(cells.size())) {
        if (auto total = parse_decimal(cells[static_cast<std::size_t>(total_cost_idx)])) {
          cost_basis_total = *total;
        }
      } else if (avg_cost_idx >= 0 && avg_cost_idx < static_cast<int>(cells.size())) {
        if (auto avg = parse_decimal(cells[static_cast<std::size_t>(avg_cost_idx)])) {
          cost_basis_total = (*avg) * (*quantity);
        }
      }
      raw_positions.push_back(PositionEntry{symbol, *quantity, cost_basis_total, source});
    }
  }

  std::unordered_map<std::string, PositionEntry> aggregated;
  for (const auto& entry : raw_positions) {
    auto& bucket = aggregated[entry.symbol];
    if (bucket.symbol.empty()) {
      bucket.symbol = entry.symbol;
      bucket.source = entry.source;
    }
    bucket.quantity += entry.quantity;
    bucket.cost_basis_total += entry.cost_basis_total;
  }

  std::vector<PositionEntry> positions;
  positions.reserve(aggregated.size());
  for (auto& [_, entry] : aggregated) {
    positions.push_back(std::move(entry));
  }
  std::sort(positions.begin(), positions.end(), [](const auto& lhs, const auto& rhs) { return lhs.symbol < rhs.symbol; });
  return positions;
}

std::vector<BalanceEntry> load_balances_from_csv(const std::filesystem::path& root,
                                                 const std::vector<std::string>& sources) {
  std::vector<BalanceEntry> balances;
  for (const auto& source : sources) {
    if (std::filesystem::path(source).extension() != ".csv") {
      continue;
    }
    std::ifstream in(root / source);
    if (!in) {
      continue;
    }
    std::string header_line;
    if (!std::getline(in, header_line)) {
      continue;
    }
    auto headers = split_csv_row(header_line);
    for (auto& header : headers) {
      header = lower_copy(header);
    }
    const int amount_idx = header_index(headers, {"amount", "balance", "cash", "value"});
    const int label_idx = header_index(headers, {"label", "account", "name", "bucket"});
    if (amount_idx < 0 || label_idx < 0) {
      continue;
    }
    if (header_index(headers, {"symbol", "ticker", "asset"}) >= 0 &&
        header_index(headers, {"quantity", "qty", "shares", "units"}) >= 0) {
      continue;
    }
    const int currency_idx = header_index(headers, {"currency", "ccy"});
    std::string row;
    while (std::getline(in, row)) {
      auto cells = split_csv_row(row);
      if (amount_idx >= static_cast<int>(cells.size()) || label_idx >= static_cast<int>(cells.size())) {
        continue;
      }
      auto amount = parse_decimal(cells[static_cast<std::size_t>(amount_idx)]);
      auto label = trim_copy(cells[static_cast<std::size_t>(label_idx)]);
      if (!amount || label.empty()) {
        continue;
      }
      BalanceEntry entry;
      entry.label = label;
      entry.amount = *amount;
      entry.source = source;
      if (currency_idx >= 0 && currency_idx < static_cast<int>(cells.size())) {
        auto currency = trim_copy(cells[static_cast<std::size_t>(currency_idx)]);
        if (!currency.empty()) {
          entry.currency = currency;
        }
      }
      balances.push_back(std::move(entry));
    }
  }
  return balances;
}

std::unordered_map<std::string, MarketQuote> load_local_quotes_from_csv(const std::filesystem::path& root,
                                                                        const std::vector<std::string>& sources) {
  std::unordered_map<std::string, MarketQuote> quotes;
  for (const auto& source : sources) {
    if (std::filesystem::path(source).extension() != ".csv") {
      continue;
    }
    std::ifstream in(root / source);
    if (!in) {
      continue;
    }
    std::string header_line;
    if (!std::getline(in, header_line)) {
      continue;
    }
    auto headers = split_csv_row(header_line);
    for (auto& header : headers) {
      header = lower_copy(header);
    }
    const int symbol_idx = header_index(headers, {"symbol", "ticker", "asset"});
    const int price_idx = header_index(headers, {"price", "last_price", "last", "close"});
    if (symbol_idx < 0 || price_idx < 0) {
      continue;
    }
    const int change_idx = header_index(headers, {"change", "daily_change"});
    const int percent_idx = header_index(headers, {"percent_change", "pct_change", "change_percent"});
    std::string row;
    while (std::getline(in, row)) {
      auto cells = split_csv_row(row);
      if (symbol_idx >= static_cast<int>(cells.size()) || price_idx >= static_cast<int>(cells.size())) {
        continue;
      }
      auto symbol = trim_copy(cells[static_cast<std::size_t>(symbol_idx)]);
      std::transform(symbol.begin(),
                     symbol.end(),
                     symbol.begin(),
                     [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
      auto price = parse_decimal(cells[static_cast<std::size_t>(price_idx)]);
      if (symbol.empty() || !price) {
        continue;
      }
      MarketQuote quote;
      quote.symbol = symbol;
      quote.has_data = *price > 0.0;
      quote.last_price = *price;
      quote.provider = "local-csv";
      quote.status = quote.has_data ? "ok" : "provider returned no price";
      if (change_idx >= 0 && change_idx < static_cast<int>(cells.size())) {
        if (auto value = parse_decimal(cells[static_cast<std::size_t>(change_idx)])) {
          quote.change = *value;
        }
      }
      if (percent_idx >= 0 && percent_idx < static_cast<int>(cells.size())) {
        if (auto value = parse_decimal(cells[static_cast<std::size_t>(percent_idx)])) {
          quote.percent_change = *value;
        }
      }
      quotes[symbol] = quote;
    }
  }
  return quotes;
}

std::unordered_map<std::string, std::vector<MarketCandle>> load_local_candles_from_csv(
    const std::filesystem::path& root,
    const std::vector<std::string>& sources) {
  std::unordered_map<std::string, std::vector<MarketCandle>> candles;
  for (const auto& source : sources) {
    if (std::filesystem::path(source).extension() != ".csv") {
      continue;
    }
    std::ifstream in(root / source);
    std::string header_line;
    if (!in || !std::getline(in, header_line)) {
      continue;
    }
    auto headers = split_csv_row(header_line);
    for (auto& header : headers) {
      header = lower_copy(header);
    }
    const int symbol_idx = header_index(headers, {"symbol", "ticker", "asset"});
    const int datetime_idx = header_index(headers, {"datetime", "date", "timestamp", "time"});
    const int open_idx = header_index(headers, {"open"});
    const int high_idx = header_index(headers, {"high"});
    const int low_idx = header_index(headers, {"low"});
    const int close_idx = header_index(headers, {"close"});
    const int volume_idx = header_index(headers, {"volume"});
    if (symbol_idx < 0 || datetime_idx < 0 || open_idx < 0 || high_idx < 0 || low_idx < 0 || close_idx < 0) {
      continue;
    }
    std::string row;
    while (std::getline(in, row)) {
      const auto cells = split_csv_row(row);
      const auto max_required = std::max({symbol_idx, datetime_idx, open_idx, high_idx, low_idx, close_idx});
      if (max_required >= static_cast<int>(cells.size())) {
        continue;
      }
      auto symbol = trim_copy(cells[static_cast<std::size_t>(symbol_idx)]);
      std::transform(symbol.begin(), symbol.end(), symbol.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
      });
      const auto open = parse_decimal(cells[static_cast<std::size_t>(open_idx)]);
      const auto high = parse_decimal(cells[static_cast<std::size_t>(high_idx)]);
      const auto low = parse_decimal(cells[static_cast<std::size_t>(low_idx)]);
      const auto close = parse_decimal(cells[static_cast<std::size_t>(close_idx)]);
      if (symbol.empty() || !open || !high || !low || !close) {
        continue;
      }
      MarketCandle candle{cells[static_cast<std::size_t>(datetime_idx)], *open, *high, *low, *close, 0.0};
      if (volume_idx >= 0 && volume_idx < static_cast<int>(cells.size())) {
        candle.volume = parse_decimal(cells[static_cast<std::size_t>(volume_idx)]).value_or(0.0);
      }
      candles[symbol].push_back(std::move(candle));
    }
  }
  for (auto& [_, series] : candles) {
    std::sort(series.begin(), series.end(), [](const MarketCandle& lhs, const MarketCandle& rhs) {
      return lhs.datetime < rhs.datetime;
    });
    if (series.size() > 30) {
      series.erase(series.begin(), series.end() - 30);
    }
  }
  return candles;
}

std::vector<MarketCandle> fetch_twelve_data_candles(const std::filesystem::path& root,
                                                     const std::string& token,
                                                     const std::string& symbol) {
  ProcessRunner runner;
  ProcessRequest request;
  request.cwd = root;
  request.argv = {
      "curl", "--silent", "--show-error", "--fail",
      "https://api.twelvedata.com/time_series?symbol=" + symbol +
          "&interval=1day&outputsize=30&format=CSV&apikey=" + token,
  };
  request.timeout = std::chrono::milliseconds(5000);
  const auto result = runner.run(request);
  if (result.exit_code != 0) {
    return {};
  }
  std::istringstream in(result.stdout_text);
  std::string header_line;
  if (!std::getline(in, header_line)) {
    return {};
  }
  auto headers = split_csv_row(header_line);
  for (auto& header : headers) {
    header = lower_copy(header);
  }
  const int datetime_idx = header_index(headers, {"datetime", "date"});
  const int open_idx = header_index(headers, {"open"});
  const int high_idx = header_index(headers, {"high"});
  const int low_idx = header_index(headers, {"low"});
  const int close_idx = header_index(headers, {"close"});
  const int volume_idx = header_index(headers, {"volume"});
  if (datetime_idx < 0 || open_idx < 0 || high_idx < 0 || low_idx < 0 || close_idx < 0) {
    return {};
  }
  std::vector<MarketCandle> candles;
  std::string row;
  while (std::getline(in, row)) {
    const auto cells = split_csv_row(row);
    const auto max_required = std::max({datetime_idx, open_idx, high_idx, low_idx, close_idx});
    if (max_required >= static_cast<int>(cells.size())) {
      continue;
    }
    const auto open = parse_decimal(cells[static_cast<std::size_t>(open_idx)]);
    const auto high = parse_decimal(cells[static_cast<std::size_t>(high_idx)]);
    const auto low = parse_decimal(cells[static_cast<std::size_t>(low_idx)]);
    const auto close = parse_decimal(cells[static_cast<std::size_t>(close_idx)]);
    if (!open || !high || !low || !close) {
      continue;
    }
    MarketCandle candle{cells[static_cast<std::size_t>(datetime_idx)], *open, *high, *low, *close, 0.0};
    if (volume_idx >= 0 && volume_idx < static_cast<int>(cells.size())) {
      candle.volume = parse_decimal(cells[static_cast<std::size_t>(volume_idx)]).value_or(0.0);
    }
    candles.push_back(std::move(candle));
  }
  std::sort(candles.begin(), candles.end(), [](const MarketCandle& lhs, const MarketCandle& rhs) {
    return lhs.datetime < rhs.datetime;
  });
  return candles;
}

MarketQuote quote_from_candles(const std::string& symbol,
                               const std::vector<MarketCandle>& candles,
                               const std::string& provider) {
  MarketQuote quote;
  quote.symbol = symbol;
  quote.provider = provider;
  if (candles.empty()) {
    quote.status = "no candle data returned";
    return quote;
  }
  quote.has_data = true;
  quote.last_price = candles.back().close;
  if (candles.size() > 1) {
    quote.change = candles.back().close - candles[candles.size() - 2].close;
    if (candles[candles.size() - 2].close != 0.0) {
      quote.percent_change = quote.change * 100.0 / candles[candles.size() - 2].close;
    }
  }
  quote.status = "ok";
  return quote;
}

std::optional<AlertRule> parse_alert_rule_line(std::string line, const std::string& source) {
  auto cleaned = trim_copy(std::move(line));
  if (cleaned.empty() || cleaned[0] == '#') {
    return std::nullopt;
  }

  std::istringstream row(cleaned);
  std::string symbol;
  std::string op;
  std::string threshold_token;
  if (!(row >> symbol >> op >> threshold_token)) {
    return std::nullopt;
  }
  auto threshold = parse_decimal(threshold_token);
  if (!threshold) {
    return std::nullopt;
  }

  AlertRule rule;
  std::transform(symbol.begin(),
                 symbol.end(),
                 symbol.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
  rule.symbol = symbol;
  rule.threshold = *threshold;
  rule.source = source;
  rule.direction = (op == "<=" || op == "<") ? AlertDirection::BelowOrEqual : AlertDirection::AboveOrEqual;
  std::getline(row, rule.note);
  rule.note = trim_copy(rule.note);
  return rule;
}

std::vector<AlertRule> load_alert_rules(const std::filesystem::path& root) {
  std::vector<AlertRule> rules;
  std::ifstream in(root / ".deck" / "alerts.txt");
  if (!in) {
    return rules;
  }
  std::string line;
  while (std::getline(in, line)) {
    if (auto rule = parse_alert_rule_line(line, ".deck/alerts.txt")) {
      rules.push_back(std::move(*rule));
    }
  }
  return rules;
}

bool persist_alert_rules(const std::filesystem::path& root, const std::vector<AlertRule>& rules) {
  std::error_code ec;
  std::filesystem::create_directories(root / ".deck", ec);
  std::ofstream out(root / ".deck" / "alerts.txt", std::ios::trunc);
  if (!out) {
    return false;
  }
  for (const auto& rule : rules) {
    out << rule.symbol << " " << (rule.direction == AlertDirection::AboveOrEqual ? ">=" : "<=") << " "
        << rule.threshold;
    if (!rule.note.empty()) {
      out << " " << rule.note;
    }
    out << "\n";
  }
  return true;
}

std::vector<TriggeredAlert> evaluate_alerts(const std::vector<AlertRule>& rules,
                                            const std::unordered_map<std::string, MarketQuote>& quotes) {
  std::vector<TriggeredAlert> triggered;
  for (const auto& rule : rules) {
    auto found = quotes.find(rule.symbol);
    if (found == quotes.end() || !found->second.has_data) {
      continue;
    }
    const auto price = found->second.last_price;
    const bool matches = rule.direction == AlertDirection::AboveOrEqual ? price >= rule.threshold
                                                                        : price <= rule.threshold;
    if (!matches) {
      continue;
    }
    std::ostringstream message;
    message << rule.symbol << " "
            << (rule.direction == AlertDirection::AboveOrEqual ? ">=" : "<=") << " " << std::fixed
            << std::setprecision(2) << rule.threshold << " at " << price;
    if (!rule.note.empty()) {
      message << "  " << rule.note;
    }
    triggered.push_back({rule.symbol, message.str(), rule.source});
  }
  return triggered;
}

std::string market_provider_label(bool has_local, bool has_twelve_data, bool has_finnhub) {
  std::string label;
  const auto append = [&](const std::string& provider) {
    if (!label.empty()) {
      label += "+";
    }
    label += provider;
  };
  if (has_local) {
    append("local-csv");
  }
  if (has_twelve_data) {
    append("twelve-data");
  }
  if (has_finnhub) {
    append("finnhub");
  }
  return label.empty() ? "none" : label;
}

std::vector<std::string> capability_notes_for(const WorkspaceRuntimeState& runtime,
                                              const EnvironmentCapabilities& caps,
                                              TabRole role,
                                              bool safe_mode) {
  std::vector<std::string> notes;
  if (safe_mode) {
    notes.push_back("Safe mode disables the fullscreen shell and background refresh workers.");
    notes.push_back("Use `deck` without `--safe` for interactive panes, overlays, and periodic quote refresh.");
  }
  if (role == TabRole::Dev || role == TabRole::Run) {
    if (!caps.rg) {
      notes.push_back("Search is degraded because `rg` is missing from PATH.");
    }
  }
  if (role == TabRole::Review && !caps.git) {
    notes.push_back("Review actions are degraded because `git` is missing from PATH.");
  }
  if (role == TabRole::Finance) {
    if (!runtime.market_data_enabled) {
      if (!caps.curl) {
        notes.push_back("Market data is disabled because local OHLC data is missing and `curl` is unavailable.");
      } else if (!caps.twelve_data_api_key && !caps.finnhub_api_key) {
        notes.push_back("Market data is disabled until local OHLC data or a supported API key is detected.");
      } else {
        notes.push_back("Finance quotes are disabled until a watchlist symbol matches a working provider.");
      }
    } else if (runtime.market_data_provider == "local-csv") {
      notes.push_back("Finance quotes currently come from local CSV data only.");
    } else if (runtime.market_data_provider == "none") {
      notes.push_back("Finance sources were found, but none currently provide live prices for the watchlist.");
    }
    if (runtime.alert_rules.empty()) {
      notes.push_back("No alerts loaded. Add `.deck/alerts.txt` to enable threshold tracking.");
    }
  }
  if (notes.empty()) {
    notes.push_back("All required capabilities for this tab are currently available.");
  }
  return notes;
}

std::vector<std::string> portfolio_lines_for(const WorkspaceRuntimeState& runtime) {
  std::vector<std::string> lines;
  lines.push_back("Focused symbol: " + (runtime.current_market_symbol.empty() ? std::string("none")
                                                                             : runtime.current_market_symbol));
  lines.push_back("Watchlist size: " + std::to_string(runtime.market_entries.size()));
  lines.push_back("Market data: " + (runtime.market_data_enabled ? runtime.market_data_provider : std::string("disabled")));
  lines.push_back("Data sources: " + std::to_string(runtime.finance_data_sources.size()));
  lines.push_back("Positions: " + std::to_string(runtime.positions.size()) + "  balances: " +
                  std::to_string(runtime.balances.size()));
  lines.push_back("Alerts: " + std::to_string(runtime.alert_rules.size()) + "  triggered: " +
                  std::to_string(runtime.triggered_alerts.size()));
  if (!runtime.finance_data_sources.empty()) {
    lines.push_back("Primary source: " + runtime.finance_data_sources.front());
  } else {
    lines.push_back("Primary source: none discovered");
  }

  double cost_basis_total = 0.0;
  double market_value_total = 0.0;
  double daily_change_total = 0.0;
  double cash_total = 0.0;
  double quoted_cost_basis_total = 0.0;
  std::size_t positions_with_quotes = 0;
  std::optional<std::pair<std::string, double>> best_daily_mover;
  std::optional<std::pair<std::string, double>> worst_daily_mover;
  std::optional<std::pair<std::string, double>> largest_holding;
  std::optional<std::pair<std::string, double>> best_unrealized;
  std::optional<std::pair<std::string, double>> worst_unrealized;
  for (const auto& position : runtime.positions) {
    cost_basis_total += position.cost_basis_total;
    if (auto found = runtime.market_quotes.find(position.symbol); found != runtime.market_quotes.end() &&
                                                             found->second.has_data) {
      const auto position_market_value = position.quantity * found->second.last_price;
      const auto position_daily_change = position.quantity * found->second.change;
      const auto position_unrealized = position_market_value - position.cost_basis_total;
      market_value_total += position_market_value;
      quoted_cost_basis_total += position.cost_basis_total;
      daily_change_total += position_daily_change;
      ++positions_with_quotes;
      if (!best_daily_mover || position_daily_change > best_daily_mover->second) {
        best_daily_mover = {position.symbol, position_daily_change};
      }
      if (!worst_daily_mover || position_daily_change < worst_daily_mover->second) {
        worst_daily_mover = {position.symbol, position_daily_change};
      }
      if (!largest_holding || position_market_value > largest_holding->second) {
        largest_holding = {position.symbol, position_market_value};
      }
      if (!best_unrealized || position_unrealized > best_unrealized->second) {
        best_unrealized = {position.symbol, position_unrealized};
      }
      if (!worst_unrealized || position_unrealized < worst_unrealized->second) {
        worst_unrealized = {position.symbol, position_unrealized};
      }
    }
  }
  for (const auto& balance : runtime.balances) {
    cash_total += balance.amount;
  }
  const auto total_tracked = market_value_total + cash_total;

  {
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(2) << "Cost basis: " << cost_basis_total
            << "  cash: " << cash_total;
    lines.push_back(summary.str());
  }
  {
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(2) << "Market value: " << market_value_total << "  total tracked: "
            << total_tracked;
    lines.push_back(summary.str());
  }
  {
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(2) << "Daily change: " << daily_change_total << "  quoted positions: "
            << positions_with_quotes << "/" << runtime.positions.size();
    lines.push_back(summary.str());
  }
  {
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(2) << "Unrealized P/L: " << (market_value_total - quoted_cost_basis_total)
            << "  quoted cost basis: " << quoted_cost_basis_total;
    lines.push_back(summary.str());
  }
  if (total_tracked > 0.0) {
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(1) << "Allocation: invested " << (market_value_total * 100.0 / total_tracked)
            << "%  cash " << (cash_total * 100.0 / total_tracked) << "%";
    lines.push_back(summary.str());
  }
  if (!runtime.positions.empty()) {
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(1) << "Quote coverage: "
            << (runtime.positions.empty() ? 0.0 : (positions_with_quotes * 100.0 / runtime.positions.size())) << "%";
    lines.push_back(summary.str());
  }

  auto focused_position = std::find_if(runtime.positions.begin(),
                                       runtime.positions.end(),
                                       [&](const PositionEntry& entry) { return entry.symbol == runtime.current_market_symbol; });
  if (focused_position != runtime.positions.end()) {
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(4) << "Focused position: " << focused_position->symbol << " qty "
            << focused_position->quantity << " cost " << focused_position->cost_basis_total;
    lines.push_back(summary.str());
    if (auto found = runtime.market_quotes.find(focused_position->symbol); found != runtime.market_quotes.end() &&
                                                                found->second.has_data) {
      const auto focused_market_value = focused_position->quantity * found->second.last_price;
      std::ostringstream pnl;
      pnl << std::fixed << std::setprecision(2) << "Focused unrealized: "
          << (focused_market_value - focused_position->cost_basis_total) << "  market value: " << focused_market_value;
      lines.push_back(pnl.str());
    }
  }

  if (best_daily_mover) {
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(2) << "Best daily mover: " << best_daily_mover->first << " "
            << best_daily_mover->second;
    lines.push_back(summary.str());
  }
  if (worst_daily_mover) {
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(2) << "Worst daily mover: " << worst_daily_mover->first << " "
            << worst_daily_mover->second;
    lines.push_back(summary.str());
  }
  if (largest_holding && market_value_total > 0.0) {
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(1) << "Largest holding: " << largest_holding->first << " "
            << largest_holding->second << " (" << (largest_holding->second * 100.0 / market_value_total) << "% of invested)";
    lines.push_back(summary.str());
  }
  if (best_unrealized) {
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(2) << "Best unrealized: " << best_unrealized->first << " "
            << best_unrealized->second;
    lines.push_back(summary.str());
  }
  if (worst_unrealized) {
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(2) << "Worst unrealized: " << worst_unrealized->first << " "
            << worst_unrealized->second;
    lines.push_back(summary.str());
  }

  if (auto found = runtime.market_quotes.find(runtime.current_market_symbol); found != runtime.market_quotes.end()) {
    const auto& quote = found->second;
    if (quote.has_data) {
      std::ostringstream line;
      line << std::fixed << std::setprecision(2) << "Last price: " << quote.last_price << "  change: " << quote.change
           << " (" << quote.percent_change << "%)";
      lines.push_back(line.str());
    } else {
      lines.push_back("Quote status: " + quote.status);
    }
  } else if (runtime.market_data_enabled) {
    lines.push_back("Quote status: waiting for first refresh");
  }
  for (std::size_t i = 0; i < runtime.triggered_alerts.size() && i < 3; ++i) {
    lines.push_back("Alert: " + runtime.triggered_alerts[i].message);
  }
  return lines;
}

bool extract_json_number(const std::string& text, const std::string& key, double& value) {
  const auto marker = "\"" + key + "\"";
  auto pos = text.find(marker);
  if (pos == std::string::npos) {
    return false;
  }
  pos = text.find(':', pos + marker.size());
  if (pos == std::string::npos) {
    return false;
  }
  ++pos;
  while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) {
    ++pos;
  }
  std::size_t end = pos;
  while (end < text.size() &&
         (std::isdigit(static_cast<unsigned char>(text[end])) || text[end] == '-' || text[end] == '+' ||
          text[end] == '.' || text[end] == 'e' || text[end] == 'E')) {
    ++end;
  }
  if (end == pos) {
    return false;
  }
  const auto token = text.substr(pos, end - pos);
  auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == token.data() + token.size();
}

MarketQuote fetch_finnhub_quote(const std::filesystem::path& root,
                                const std::string& token,
                                const std::string& symbol) {
  MarketQuote quote;
  quote.symbol = symbol;
  quote.status = "request failed";

  ProcessRunner runner;
  ProcessRequest request;
  request.cwd = root;
  request.argv = {
      "curl",
      "--silent",
      "--show-error",
      "--fail",
      "https://finnhub.io/api/v1/quote?symbol=" + symbol + "&token=" + token,
  };
  request.timeout = std::chrono::milliseconds(5000);
  const auto result = runner.run(request);
  if (result.exit_code != 0) {
    quote.status = result.stderr_text.empty() ? "curl failed" : clip_text(result.stderr_text, 80);
    return quote;
  }

  double current = 0.0;
  double change = 0.0;
  double percent = 0.0;
  double timestamp = 0.0;
  if (!extract_json_number(result.stdout_text, "c", current)) {
    quote.status = "quote parse failed";
    return quote;
  }
  extract_json_number(result.stdout_text, "d", change);
  extract_json_number(result.stdout_text, "dp", percent);
  extract_json_number(result.stdout_text, "t", timestamp);

  quote.has_data = current > 0.0;
  quote.last_price = current;
  quote.change = change;
  quote.percent_change = percent;
  quote.timestamp = static_cast<long long>(timestamp);
  quote.provider = "finnhub";
  quote.status = quote.has_data ? "ok" : "provider returned no price";
  return quote;
}

void bootstrap_runtime_state(const WorkspacePersistentState& persistent,
                             const EnvironmentCapabilities& caps,
                             bool safe_mode,
                             WorkspaceRuntimeState& runtime) {
  runtime.active_task_state = TaskState::Idle;
  runtime.files_entries = discover_file_entries(persistent.root, runtime.files_browser_root);
  runtime.market_entries = discover_market_entries(persistent.root);
  runtime.finance_data_sources = discover_finance_sources(persistent.root);
  runtime.positions = load_positions_from_csv(persistent.root, runtime.finance_data_sources);
  runtime.balances = load_balances_from_csv(persistent.root, runtime.finance_data_sources);
  runtime.alert_rules = load_alert_rules(persistent.root);
  const auto local_quotes = load_local_quotes_from_csv(persistent.root, runtime.finance_data_sources);
  runtime.market_candles = load_local_candles_from_csv(persistent.root, runtime.finance_data_sources);
  runtime.market_quotes = local_quotes;
  runtime.triggered_alerts = evaluate_alerts(runtime.alert_rules, runtime.market_quotes);
  runtime.market_data_enabled = !local_quotes.empty() || !runtime.market_candles.empty() ||
                                (caps.curl && (caps.twelve_data_api_key || caps.finnhub_api_key));
  runtime.market_data_provider = market_provider_label(!local_quotes.empty() || !runtime.market_candles.empty(),
                                                       caps.curl && caps.twelve_data_api_key,
                                                       caps.curl && caps.finnhub_api_key);
  runtime.scratch_editor.buffer = load_scratch_buffer(persistent.root);
  runtime.scratch_editor.cursor = runtime.scratch_editor.buffer.size();
  if (!runtime.market_entries.empty()) {
    runtime.current_market_symbol = runtime.market_entries.front().symbol;
  }
  refresh_note_context(persistent.root, runtime);
  runtime.portfolio_lines = portfolio_lines_for(runtime);
  if (safe_mode) {
    runtime.status_message = "Safe mode active";
    return;
  }

  ProcessRunner runner;

  if (caps.git) {
    ProcessRequest request;
    request.argv = {"git", "-C", persistent.root.string(), "status", "--short", "--branch"};
    request.cwd = persistent.root;
    request.timeout = std::chrono::milliseconds(300);
    runtime.task_history.push_back(execute_task_probe("git status", request, runner));

    ProcessRequest status_request;
    status_request.argv = {"git", "-C", persistent.root.string(), "status", "--porcelain", "--branch"};
    status_request.cwd = persistent.root;
    status_request.timeout = std::chrono::milliseconds(300);
    const auto status_result = runner.run(status_request);
    runtime.git_entries = parse_git_status_entries(status_result.stdout_text);
    runtime.git_status_text = status_result.stdout_text;
    ProcessRequest branch_request;
    branch_request.argv = {"git", "-C", persistent.root.string(), "branch", "--no-color"};
    branch_request.cwd = persistent.root;
    branch_request.timeout = std::chrono::milliseconds(300);
    const auto branch_result = runner.run(branch_request);
    runtime.git_branches = parse_git_branch_entries(branch_result.stdout_text);
    for (const auto& branch : runtime.git_branches) {
      if (branch.current) {
        runtime.current_git_branch = branch.name;
        break;
      }
    }
    ProcessRequest log_request;
    log_request.argv = {"git", "-C", persistent.root.string(), "log", "-5", "--pretty=format:%h %s"};
    log_request.cwd = persistent.root;
    log_request.timeout = std::chrono::milliseconds(300);
    const auto log_result = runner.run(log_request);
    std::istringstream log_lines(log_result.stdout_text);
    std::string log_line;
    while (std::getline(log_lines, log_line)) {
      if (!log_line.empty()) {
        runtime.git_recent_commits.push_back(log_line);
      }
    }
    if (!runtime.git_entries.empty()) {
      const auto& selected = runtime.git_entries.front();
      const auto staged = selected.index_status != " " && selected.index_status != "?";
      ProcessRequest diff_request;
      diff_request.argv = staged
                              ? std::vector<std::string>{"git", "-C", persistent.root.string(), "diff", "--cached", "--", selected.path}
                              : std::vector<std::string>{"git", "-C", persistent.root.string(), "diff", "--", selected.path};
      diff_request.cwd = persistent.root;
      diff_request.timeout = std::chrono::milliseconds(300);
      const auto diff_result = runner.run(diff_request);
      runtime.diff_hunks = parse_diff_hunks(diff_result.stdout_text);
      runtime.diff_preview_text = diff_result.stdout_text;
    }
  }

  if (caps.rg) {
    ProcessRequest request;
    request.argv = {"rg", "--files"};
    request.cwd = persistent.root;
    request.timeout = std::chrono::milliseconds(300);
    runtime.task_history.push_back(execute_task_probe("workspace files", request, runner));
  }
  runtime.status_message = "Ready";
}

void refresh_market_quotes(ScreenInteractive& screen,
                           ShellTaskController& controller,
                           const WorkspacePersistentState& persistent,
                           const EnvironmentCapabilities& caps,
                           bool refresh_claimed) {
  const auto finnhub_token = (caps.curl && caps.finnhub_api_key)
                                 ? resolve_env_var(persistent.root, "FINNHUB_API_KEY")
                                 : std::nullopt;
  const auto twelve_data_token = (caps.curl && caps.twelve_data_api_key)
                                     ? resolve_env_var(persistent.root, "TWELVE_DATA_API_KEY")
                                     : std::nullopt;
  const auto sources = discover_finance_sources(persistent.root);
  const auto local_quotes = load_local_quotes_from_csv(persistent.root, sources);
  const auto local_candles = load_local_candles_from_csv(persistent.root, sources);
  if (local_quotes.empty() && local_candles.empty() && !twelve_data_token && !finnhub_token) {
    if (refresh_claimed) {
      std::lock_guard<std::mutex> lock(controller.mutex);
      controller.runtime.market_data_refresh_in_progress = false;
    }
    if (!caps.curl) {
      set_status(controller, "No local OHLC CSV found and curl is unavailable for market refresh");
    } else if (!caps.twelve_data_api_key && !caps.finnhub_api_key) {
      set_status(controller, "No local OHLC CSV or market API key found; run doctor");
    } else {
      set_status(controller, "No local quote CSV or working Finnhub token found");
    }
    return;
  }

  std::vector<MarketEntry> watchlist;
  std::unordered_map<std::string, std::vector<MarketCandle>> candles;
  std::string focused_symbol;
  {
    std::lock_guard<std::mutex> lock(controller.mutex);
    watchlist = controller.runtime.market_entries;
    candles = controller.runtime.market_candles;
    focused_symbol = controller.runtime.current_market_symbol;
  }
  for (const auto& [symbol, series] : local_candles) {
    candles[symbol] = series;
  }
  screen.PostEvent(ftxui::Event::Custom);

  std::unordered_map<std::string, MarketQuote> quotes;
  bool used_local = false;
  bool used_twelve_data = false;
  bool used_finnhub = false;
  for (const auto& market : watchlist) {
    if (auto found = local_candles.find(market.symbol); found != local_candles.end() && !found->second.empty()) {
      quotes[market.symbol] = quote_from_candles(market.symbol, found->second, "local-csv");
      used_local = true;
      continue;
    }
    if (twelve_data_token && market.symbol == focused_symbol) {
      auto series = fetch_twelve_data_candles(persistent.root, *twelve_data_token, market.symbol);
      if (!series.empty()) {
        quotes[market.symbol] = quote_from_candles(market.symbol, series, "twelve-data");
        candles[market.symbol] = std::move(series);
        used_twelve_data = true;
        continue;
      }
    }
    if (auto found = candles.find(market.symbol); found != candles.end() && !found->second.empty()) {
      quotes[market.symbol] = quote_from_candles(market.symbol, found->second, "twelve-data-cache");
      used_twelve_data = true;
      continue;
    }
    if (auto found = local_quotes.find(market.symbol); found != local_quotes.end()) {
      quotes[market.symbol] = found->second;
      used_local = true;
      continue;
    }
    if (finnhub_token) {
      quotes[market.symbol] = fetch_finnhub_quote(persistent.root, *finnhub_token, market.symbol);
      used_finnhub = true;
      continue;
    }
    MarketQuote quote;
    quote.symbol = market.symbol;
    quote.status = "no provider for symbol";
    quotes[market.symbol] = quote;
  }

  {
    std::lock_guard<std::mutex> lock(controller.mutex);
    controller.runtime.market_quotes = std::move(quotes);
    controller.runtime.market_candles = std::move(candles);
    controller.runtime.triggered_alerts = evaluate_alerts(controller.runtime.alert_rules, controller.runtime.market_quotes);
    controller.runtime.market_data_enabled = used_local || used_twelve_data || used_finnhub;
    controller.runtime.market_data_provider = market_provider_label(used_local, used_twelve_data, used_finnhub);
    controller.runtime.market_data_refresh_in_progress = false;
    controller.runtime.portfolio_lines = portfolio_lines_for(controller.runtime);
    controller.runtime.status_message = controller.runtime.triggered_alerts.empty()
                                            ? "Market data refreshed"
                                            : "Alert: " + controller.runtime.triggered_alerts.front().message;
  }
  invalidate_pane_data_snapshot(persistent.root);
  screen.PostEvent(ftxui::Event::Custom);
}

void request_market_quotes(ScreenInteractive& screen,
                           ShellTaskController& controller,
                           const WorkspacePersistentState& persistent,
                           const EnvironmentCapabilities& caps) {
  {
    std::lock_guard<std::mutex> lock(controller.mutex);
    if (controller.runtime.market_data_refresh_in_progress) {
      controller.runtime.status_message = "Market refresh already running";
      screen.PostEvent(ftxui::Event::Custom);
      return;
    }
    controller.runtime.market_data_refresh_in_progress = true;
    controller.runtime.status_message = "Refreshing market data…";
  }
  invalidate_pane_data_snapshot(persistent.root);
  screen.PostEvent(ftxui::Event::Custom);
  controller.market_worker = std::jthread([&] {
    refresh_market_quotes(screen, controller, persistent, caps, true);
  });
}

void add_watchlist_symbol(ScreenInteractive& screen,
                          ShellTaskController& controller,
                          const WorkspacePersistentState& persistent,
                          const EnvironmentCapabilities& caps,
                          std::string symbol) {
  symbol.erase(std::remove_if(symbol.begin(),
                              symbol.end(),
                              [](unsigned char ch) { return std::isspace(ch); }),
               symbol.end());
  std::transform(symbol.begin(), symbol.end(), symbol.begin(), [](unsigned char ch) { return std::toupper(ch); });
  if (symbol.empty()) {
    set_status(controller, "Ticker input was empty");
    return;
  }

  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(controller.mutex);
    auto exists = std::find_if(controller.runtime.market_entries.begin(),
                               controller.runtime.market_entries.end(),
                               [&](const MarketEntry& entry) { return entry.symbol == symbol; });
    if (exists == controller.runtime.market_entries.end()) {
      controller.runtime.market_entries.push_back({symbol, "watchlist"});
      controller.runtime.selected_market_index = controller.runtime.market_entries.size() - 1;
      controller.runtime.current_market_symbol = symbol;
      controller.runtime.portfolio_lines = portfolio_lines_for(controller.runtime);
      refresh_note_context(persistent.root, controller.runtime);
      controller.runtime.status_message = "Added " + symbol;
      changed = true;
    } else {
      controller.runtime.selected_market_index =
          static_cast<std::size_t>(std::distance(controller.runtime.market_entries.begin(), exists));
      controller.runtime.current_market_symbol = symbol;
      controller.runtime.portfolio_lines = portfolio_lines_for(controller.runtime);
      refresh_note_context(persistent.root, controller.runtime);
      controller.runtime.status_message = symbol + " already tracked";
    }
  }

  if (changed) {
    std::vector<MarketEntry> entries;
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      entries = controller.runtime.market_entries;
    }
    persist_watchlist(persistent.root, entries);
  }

  invalidate_pane_data_snapshot(persistent.root);
  screen.PostEvent(ftxui::Event::Custom);
  if (changed) {
    request_market_quotes(screen, controller, persistent, caps);
  }
}

void remove_watchlist_symbol(ScreenInteractive& screen,
                             ShellTaskController& controller,
                             const WorkspacePersistentState& persistent,
                             std::optional<std::string> requested_symbol = std::nullopt) {
  std::vector<MarketEntry> entries;
  std::string removed_symbol;
  {
    std::lock_guard<std::mutex> lock(controller.mutex);
    if (controller.runtime.market_entries.empty()) {
      controller.runtime.status_message = "Watchlist is empty";
    } else if (controller.runtime.market_entries.size() == 1) {
      controller.runtime.status_message = "Keep at least one ticker in the watchlist";
    } else {
      auto selected = controller.runtime.selected_market_index;
      if (requested_symbol) {
        std::transform(requested_symbol->begin(),
                       requested_symbol->end(),
                       requested_symbol->begin(),
                       [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        const auto found = std::find_if(controller.runtime.market_entries.begin(),
                                        controller.runtime.market_entries.end(),
                                        [&](const MarketEntry& entry) { return entry.symbol == *requested_symbol; });
        if (found == controller.runtime.market_entries.end()) {
          controller.runtime.status_message = *requested_symbol + " is not in the watchlist";
          screen.PostEvent(ftxui::Event::Custom);
          return;
        }
        selected = static_cast<std::size_t>(std::distance(controller.runtime.market_entries.begin(), found));
      }
      removed_symbol = controller.runtime.market_entries[selected].symbol;
      controller.runtime.market_entries.erase(controller.runtime.market_entries.begin() + selected);
      controller.runtime.market_quotes.erase(removed_symbol);
      controller.runtime.market_candles.erase(removed_symbol);
      controller.runtime.triggered_alerts =
          evaluate_alerts(controller.runtime.alert_rules, controller.runtime.market_quotes);
      if (controller.runtime.market_entries.empty()) {
        controller.runtime.selected_market_index = 0;
        controller.runtime.current_market_symbol.clear();
      } else {
        controller.runtime.selected_market_index =
            std::min(selected, controller.runtime.market_entries.size() - 1);
        controller.runtime.current_market_symbol =
            controller.runtime.market_entries[controller.runtime.selected_market_index].symbol;
      }
      controller.runtime.portfolio_lines = portfolio_lines_for(controller.runtime);
      refresh_note_context(persistent.root, controller.runtime);
      controller.runtime.status_message = "Removed " + removed_symbol;
      entries = controller.runtime.market_entries;
    }
  }
  if (!removed_symbol.empty() && !persist_watchlist(persistent.root, entries)) {
    set_status(controller, "Removed " + removed_symbol + ", but failed to persist watchlist");
  }
  invalidate_pane_data_snapshot(persistent.root);
  screen.PostEvent(ftxui::Event::Custom);
}

void add_alert_rule(ScreenInteractive& screen,
                    ShellTaskController& controller,
                    const WorkspacePersistentState& persistent,
                    const EnvironmentCapabilities& caps,
                    std::string rule_text) {
  auto parsed = parse_alert_rule_line(std::move(rule_text), ".deck/alerts.txt");
  if (!parsed) {
    set_status(controller, "Usage: <SYMBOL> >= <price> [note]");
    screen.PostEvent(ftxui::Event::Custom);
    return;
  }

  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(controller.mutex);
    auto duplicate = std::find_if(controller.runtime.alert_rules.begin(),
                                  controller.runtime.alert_rules.end(),
                                  [&](const AlertRule& rule) {
                                    return rule.symbol == parsed->symbol && rule.direction == parsed->direction &&
                                           rule.threshold == parsed->threshold && rule.note == parsed->note;
                                  });
    if (duplicate == controller.runtime.alert_rules.end()) {
      controller.runtime.alert_rules.push_back(*parsed);
      controller.runtime.triggered_alerts =
          evaluate_alerts(controller.runtime.alert_rules, controller.runtime.market_quotes);
      controller.runtime.portfolio_lines = portfolio_lines_for(controller.runtime);
      controller.runtime.status_message = "Added alert for " + parsed->symbol;
      changed = true;
    } else {
      controller.runtime.status_message = "Alert already exists for " + parsed->symbol;
    }
  }

  if (changed) {
    std::vector<AlertRule> rules;
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      rules = controller.runtime.alert_rules;
    }
    if (!persist_alert_rules(persistent.root, rules)) {
      set_status(controller, "Failed to persist alerts");
    } else {
      request_market_quotes(screen, controller, persistent, caps);
    }
  }

  invalidate_pane_data_snapshot(persistent.root);
  screen.PostEvent(ftxui::Event::Custom);
}

void set_status(ShellTaskController& controller, const std::string& message) {
  std::lock_guard<std::mutex> lock(controller.mutex);
  controller.runtime.status_message = message;
}

void refresh_git_state(ShellTaskController& controller,
                       const WorkspacePersistentState& persistent,
                       const EnvironmentCapabilities& caps) {
  if (!caps.git) {
    std::lock_guard<std::mutex> lock(controller.mutex);
    controller.runtime.git_entries.clear();
    controller.runtime.selected_git_index = 0;
    controller.runtime.git_branches.clear();
    controller.runtime.current_git_branch.clear();
    controller.runtime.git_recent_commits.clear();
    controller.runtime.diff_hunks.clear();
    controller.runtime.selected_diff_hunk = 0;
    controller.runtime.git_status_text.clear();
    controller.runtime.diff_preview_text.clear();
    return;
  }

  ProcessRunner runner;
  ProcessRequest request;
  request.argv = {"git", "-C", persistent.root.string(), "status", "--porcelain", "--branch"};
  request.cwd = persistent.root;
  request.timeout = std::chrono::milliseconds(500);
  const auto result = runner.run(request);

  std::size_t selected_git_index = 0;
  {
    std::lock_guard<std::mutex> lock(controller.mutex);
    selected_git_index = controller.runtime.selected_git_index;
  }

  std::vector<GitStatusEntry> git_entries = parse_git_status_entries(result.stdout_text);
  ProcessRequest branch_request;
  branch_request.argv = {"git", "-C", persistent.root.string(), "branch", "--no-color"};
  branch_request.cwd = persistent.root;
  branch_request.timeout = std::chrono::milliseconds(500);
  const auto branch_result = runner.run(branch_request);
  auto git_branches = parse_git_branch_entries(branch_result.stdout_text);
  std::string current_git_branch;
  for (const auto& branch : git_branches) {
    if (branch.current) {
      current_git_branch = branch.name;
      break;
    }
  }
  ProcessRequest log_request;
  log_request.argv = {"git", "-C", persistent.root.string(), "log", "-5", "--pretty=format:%h %s"};
  log_request.cwd = persistent.root;
  log_request.timeout = std::chrono::milliseconds(500);
  const auto log_result = runner.run(log_request);
  std::vector<std::string> git_recent_commits;
  std::istringstream log_lines(log_result.stdout_text);
  std::string log_line;
  while (std::getline(log_lines, log_line)) {
    if (!log_line.empty()) {
      git_recent_commits.push_back(log_line);
    }
  }
  std::vector<DiffHunk> diff_hunks;
  std::string diff_preview_text;
  if (!git_entries.empty()) {
    const auto& selected = git_entries[std::min(selected_git_index, git_entries.size() - 1)];
    const auto staged = selected.index_status != " " && selected.index_status != "?";
    ProcessRequest diff_request;
    diff_request.argv = staged
                            ? std::vector<std::string>{"git", "-C", persistent.root.string(), "diff", "--cached", "--", selected.path}
                            : std::vector<std::string>{"git", "-C", persistent.root.string(), "diff", "--", selected.path};
    diff_request.cwd = persistent.root;
    diff_request.timeout = std::chrono::milliseconds(500);
    const auto diff_result = runner.run(diff_request);
    diff_hunks = parse_diff_hunks(diff_result.stdout_text);
    diff_preview_text = diff_result.stdout_text;
  }

  std::lock_guard<std::mutex> lock(controller.mutex);
  controller.runtime.git_entries = std::move(git_entries);
  controller.runtime.git_branches = std::move(git_branches);
  controller.runtime.current_git_branch = std::move(current_git_branch);
  controller.runtime.git_recent_commits = std::move(git_recent_commits);
  controller.runtime.git_status_text = result.stdout_text;
  if (controller.runtime.git_entries.empty()) {
    controller.runtime.selected_git_index = 0;
  } else if (controller.runtime.selected_git_index >= controller.runtime.git_entries.size()) {
    controller.runtime.selected_git_index = controller.runtime.git_entries.size() - 1;
  }
  controller.runtime.diff_hunks = std::move(diff_hunks);
  controller.runtime.diff_preview_text = std::move(diff_preview_text);
  if (controller.runtime.diff_hunks.empty()) {
    controller.runtime.selected_diff_hunk = 0;
  } else if (controller.runtime.selected_diff_hunk >= controller.runtime.diff_hunks.size()) {
    controller.runtime.selected_diff_hunk = controller.runtime.diff_hunks.size() - 1;
  }
}

void request_git_diff_refresh(ScreenInteractive& screen,
                              ShellTaskController& controller,
                              const WorkspacePersistentState& persistent,
                              const EnvironmentCapabilities& caps) {
  if (!caps.git) {
    return;
  }
  ++controller.git_diff_generation;
  if (controller.git_diff_worker_running.exchange(true)) {
    return;
  }

  controller.git_diff_worker = std::jthread([&] {
    for (;;) {
      const auto generation = controller.git_diff_generation.load();
      std::optional<GitStatusEntry> selected;
      {
        std::lock_guard<std::mutex> lock(controller.mutex);
        if (!controller.runtime.git_entries.empty() &&
            controller.runtime.selected_git_index < controller.runtime.git_entries.size()) {
          selected = controller.runtime.git_entries[controller.runtime.selected_git_index];
        }
      }

      std::string diff_preview;
      std::vector<DiffHunk> diff_hunks;
      if (selected) {
        const auto staged = selected->index_status != " " && selected->index_status != "?";
        ProcessRunner runner;
        ProcessRequest request;
        request.argv = staged
                           ? std::vector<std::string>{"git", "-C", persistent.root.string(), "diff", "--cached", "--", selected->path}
                           : std::vector<std::string>{"git", "-C", persistent.root.string(), "diff", "--", selected->path};
        request.cwd = persistent.root;
        request.timeout = std::chrono::milliseconds(500);
        const auto result = runner.run(request);
        diff_preview = result.stdout_text;
        diff_hunks = parse_diff_hunks(diff_preview);
      }

      if (generation == controller.git_diff_generation.load()) {
        std::lock_guard<std::mutex> lock(controller.mutex);
        controller.runtime.diff_preview_text = std::move(diff_preview);
        controller.runtime.diff_hunks = std::move(diff_hunks);
        if (controller.runtime.diff_hunks.empty()) {
          controller.runtime.selected_diff_hunk = 0;
        } else if (controller.runtime.selected_diff_hunk >= controller.runtime.diff_hunks.size()) {
          controller.runtime.selected_diff_hunk = controller.runtime.diff_hunks.size() - 1;
        }
      }
      screen.PostEvent(ftxui::Event::Custom);

      if (generation == controller.git_diff_generation.load()) {
        controller.git_diff_worker_running = false;
        if (generation == controller.git_diff_generation.load()) {
          break;
        }
        if (controller.git_diff_worker_running.exchange(true)) {
          break;
        }
      }
    }
  });
}

std::vector<std::string> palette_suggestions_for(TabRole role) {
  std::vector<std::string> suggestions = {
      "run [command]",
      "rerun",
      "search <query>",
      "git-status",
      "git-diff",
      "stage <path>",
      "unstage <path>",
      "commit <message>",
      "branch <name>",
      "branch-new <name>",
      "editor <vim|nvim|vscode>",
      "clear-tasks",
      "calc <expression>",
      "cancel",
      "quit",
  };
  if (role == TabRole::Finance) {
    suggestions.push_back("refresh");
    suggestions.push_back("add <ticker>");
    suggestions.push_back("remove <ticker>");
    suggestions.push_back("alert <ticker> >= <price> [note]");
    suggestions.push_back("focus <ticker>");
  } else if (role == TabRole::Review) {
    suggestions.push_back("refresh");
    suggestions.push_back("next-file");
    suggestions.push_back("prev-file");
    suggestions.push_back("next-hunk");
    suggestions.push_back("prev-hunk");
    suggestions.push_back("stage-hunk");
    suggestions.push_back("unstage-hunk");
  } else if (role == TabRole::Run) {
    suggestions.push_back("refresh");
    suggestions.push_back("next-task");
    suggestions.push_back("prev-task");
    suggestions.push_back("rerun-task");
  } else if (role == TabRole::Math) {
    suggestions.push_back("plot-range <min> <max>");
  } else {
    suggestions.push_back("refresh");
  }
  return suggestions;
}

std::string controls_for_role(TabRole role) {
  switch (role) {
    case TabRole::Dev:
      return "/ search   :run <command> launch   j/k + Enter files   r recent command";
    case TabRole::Run:
      return "j/k select task   Enter rerun selected   R rerun latest   x cancel";
    case TabRole::Review:
      return "f files/review   j/k move   Enter open   [/] hunks   s/u file   S/U hunk   c commit";
    case TabRole::Finance:
      return "j/k ticker   Enter focus   a add   d remove   A alert   x refresh";
    case TabRole::Notes:
      return "E context note   e scratchpad   Ctrl+S save   Ctrl+R reload   Esc stop editing";
    case TabRole::Math:
      return ":calc expression   p plot   n append result to note   :plot-range min max";
  }
  return {};
}

std::vector<std::string> run_math_worker(const WorkspacePersistentState& state,
                                         const EnvironmentCapabilities& caps,
                                         const std::string& action,
                                         const std::string& expression,
                                         double minimum,
                                         double maximum) {
  if (!caps.python || !caps.sympy) {
    return {"error", "Python with SymPy is unavailable; see deck doctor"};
  }
  ProcessRequest request;
  request.argv = {caps.python_command, "-I", std::filesystem::absolute(DECK_MATH_WORKER_PATH).string()};
  request.cwd = state.root;
  request.timeout = std::chrono::milliseconds(2000);
  std::ostringstream input;
  input << action << '\0' << expression << '\0' << minimum << '\0' << maximum;
  request.stdin_text = input.str();
  const auto result = ProcessRunner{}.run(request);
  if (result.timed_out) return {"error", "Math evaluation timed out after 2 seconds"};
  if (result.exit_code != 0) return {"error", clip_text(result.stderr_text, 4096)};
  std::vector<std::string> fields;
  std::size_t cursor = 0;
  while (cursor <= result.stdout_text.size()) {
    const auto next = result.stdout_text.find('\0', cursor);
    fields.push_back(result.stdout_text.substr(cursor, next - cursor));
    if (fields.back().size() > 4096) fields.back().resize(4096);
    if (next == std::string::npos) break;
    cursor = next + 1;
  }
  return fields;
}

void launch_task(ScreenInteractive& screen,
                 ShellTaskController& controller,
                 const WorkspacePersistentState& persistent,
                 std::string name,
                 std::vector<std::string> argv,
                 bool use_pty,
                 const EnvironmentCapabilities& caps,
                 std::optional<std::string> stdin_text = std::nullopt) {
  if (argv.empty()) {
    return;
  }
  if (controller.task_running.exchange(true)) {
    return;
  }

  controller.cancel_requested = false;
  std::size_t task_index = 0;
  {
    std::lock_guard<std::mutex> lock(controller.mutex);
    controller.runtime.active_task_state = TaskState::Starting;
    constexpr std::size_t max_task_history = 64;
    if (controller.runtime.task_history.size() >= max_task_history) {
      controller.runtime.task_history.erase(controller.runtime.task_history.begin());
      if (controller.runtime.selected_task_index > 0) {
        --controller.runtime.selected_task_index;
      }
    }
    controller.runtime.task_history.push_back(make_task_record(name, argv, use_pty));
    task_index = controller.runtime.task_history.size() - 1;
    controller.runtime.selected_task_index = task_index;
    controller.active_task_index = task_index;
  }
  screen.PostEvent(ftxui::Event::Custom);

  controller.worker =
      std::jthread([&, task_index, name = std::move(name), argv = std::move(argv), use_pty, stdin_text = std::move(stdin_text)] {
    ProcessRunner runner;
    ProcessRequest request;
    request.argv = argv;
    request.cwd = persistent.root;
    request.use_pty = use_pty;
    request.stdin_text = stdin_text;

    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      if (task_index < controller.runtime.task_history.size()) {
        controller.runtime.task_history[task_index].state = TaskState::Running;
        controller.runtime.active_task_state = TaskState::Running;
      }
    }
    screen.PostEvent(ftxui::Event::Custom);

    auto result = runner.run_streaming(
        request,
        ProcessCallbacks{
            .on_stdout_chunk =
                [&](const std::string& chunk) {
                  std::lock_guard<std::mutex> lock(controller.mutex);
                  if (task_index < controller.runtime.task_history.size()) {
                    append_tail(controller.runtime.task_history[task_index].stdout_excerpt, chunk);
                  }
                  screen.PostEvent(ftxui::Event::Custom);
                },
            .on_stderr_chunk =
                [&](const std::string& chunk) {
                  std::lock_guard<std::mutex> lock(controller.mutex);
                  if (task_index < controller.runtime.task_history.size()) {
                    append_tail(controller.runtime.task_history[task_index].stderr_excerpt, chunk);
                  }
                  screen.PostEvent(ftxui::Event::Custom);
                },
            .should_cancel = [&] { return controller.cancel_requested.load(); },
        });

    std::vector<TaskRecord> persisted_tasks;
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      auto& task = controller.runtime.task_history[task_index];
      task.exit_code = result.exit_code;
      task.timed_out = result.timed_out;
      task.cancelled = result.cancelled;
      task.finished_at = format_timestamp(std::chrono::system_clock::now());
      if (result.cancelled) {
        task.state = TaskState::Cancelled;
        controller.runtime.active_task_state = TaskState::Cancelled;
      } else if (result.timed_out || result.exit_code != 0) {
        task.state = TaskState::Failed;
        controller.runtime.active_task_state = TaskState::Failed;
      } else {
        task.state = TaskState::Exited;
        controller.runtime.active_task_state = TaskState::Exited;
      }
      controller.active_task_index.reset();
      persisted_tasks = controller.runtime.task_history;
    }
    WorkspaceStore{}.save_tasks(persistent.root, persisted_tasks);
    refresh_git_state(controller, persistent, caps);
    invalidate_pane_data_snapshot(persistent.root);
    controller.task_running = false;
    if (controller.exit_requested.load()) {
      screen.ExitLoopClosure()();
    } else {
      screen.PostEvent(ftxui::Event::Custom);
    }
      });
}

std::vector<SearchResult> parse_rg_output(const std::string& text) {
  std::vector<SearchResult> results;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    auto first = line.find(':');
    if (first == std::string::npos) {
      continue;
    }
    auto second = line.find(':', first + 1);
    if (second == std::string::npos) {
      continue;
    }
    auto third = line.find(':', second + 1);
    if (third == std::string::npos) {
      continue;
    }
    SearchResult result;
    result.path = line.substr(0, first);
    result.line = std::stoi(line.substr(first + 1, second - first - 1));
    result.column = std::stoi(line.substr(second + 1, third - second - 1));
    result.preview = line.substr(third + 1);
    results.push_back(std::move(result));
  }
  return results;
}

void launch_search(ScreenInteractive& screen,
                   ShellTaskController& controller,
                   const WorkspacePersistentState& persistent,
                   const EnvironmentCapabilities& caps,
                   std::string query) {
  if (!caps.rg || query.empty()) {
    return;
  }

  std::size_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(controller.mutex);
    controller.runtime.current_search_query = query;
    controller.runtime.search_in_progress = true;
    controller.runtime.search_results.clear();
    controller.runtime.selected_search_result = 0;
    generation = ++controller.runtime.search_generation;
  }
  invalidate_pane_data_snapshot(persistent.root);
  screen.PostEvent(ftxui::Event::Custom);

  controller.search_worker = std::jthread([&, generation, query = std::move(query)] {
    ProcessRunner runner;
    ProcessRequest request;
    request.argv = {"rg", "--line-number", "--column", "--max-count", "32", query};
    request.cwd = persistent.root;
    request.timeout = std::chrono::milliseconds(1500);
    const auto result = runner.run(request);
    auto parsed = parse_rg_output(result.stdout_text);

    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      if (generation != controller.runtime.search_generation) {
        return;
      }
      controller.runtime.search_in_progress = false;
      controller.runtime.search_results = std::move(parsed);
      controller.runtime.selected_search_result = 0;
      refresh_note_context(persistent.root, controller.runtime);
    }
    invalidate_pane_data_snapshot(persistent.root);
    screen.PostEvent(ftxui::Event::Custom);
  });
}

bool run_git_action(ScreenInteractive& screen,
                    ShellTaskController& controller,
                    const WorkspacePersistentState& state,
                    const EnvironmentCapabilities& caps,
                    const std::string& name,
                    std::vector<std::string> argv) {
  if (!caps.git) {
    set_status(controller, "git is unavailable");
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }
  launch_task(screen, controller, state, name, std::move(argv), false, caps);
  return true;
}

bool stage_selected_git_entry(ScreenInteractive& screen,
                              ShellTaskController& controller,
                              const WorkspacePersistentState& state,
                              const EnvironmentCapabilities& caps) {
  std::optional<GitStatusEntry> selected;
  {
    std::lock_guard<std::mutex> lock(controller.mutex);
    if (!controller.runtime.git_entries.empty() && controller.runtime.selected_git_index < controller.runtime.git_entries.size()) {
      selected = controller.runtime.git_entries[controller.runtime.selected_git_index];
    }
  }
  if (!selected) {
    set_status(controller, "No git entry selected");
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }
  return run_git_action(
      screen, controller, state, caps, "git add", {"git", "-C", state.root.string(), "add", "--", selected->path});
}

bool apply_selected_hunk(ScreenInteractive& screen,
                         ShellTaskController& controller,
                         const WorkspacePersistentState& state,
                         const EnvironmentCapabilities& caps,
                         bool reverse) {
  std::optional<GitStatusEntry> selected;
  std::size_t selected_hunk_index = 0;
  std::string diff_preview_text;
  {
    std::lock_guard<std::mutex> lock(controller.mutex);
    if (!controller.runtime.git_entries.empty() && controller.runtime.selected_git_index < controller.runtime.git_entries.size()) {
      selected = controller.runtime.git_entries[controller.runtime.selected_git_index];
    }
    selected_hunk_index = controller.runtime.selected_diff_hunk;
    diff_preview_text = controller.runtime.diff_preview_text;
  }
  if (!selected) {
    set_status(controller, "No git entry selected");
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }

  const auto showing_staged_diff = selected->index_status != " " && selected->index_status != "?";
  if (reverse && !showing_staged_diff) {
    set_status(controller, "Selected diff is unstaged; use stage-hunk");
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }
  if (!reverse && showing_staged_diff) {
    set_status(controller, "Selected diff is staged; use unstage-hunk");
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }

  auto patch = build_patch_for_hunk(diff_preview_text, selected_hunk_index);
  if (!patch) {
    set_status(controller, "No diff hunk selected");
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }

  std::vector<std::string> argv = {"git", "-C", state.root.string(), "apply", "--cached"};
  if (reverse) {
    argv.push_back("--reverse");
  }
  argv.push_back("--");
  launch_task(screen,
              controller,
              state,
              reverse ? "git apply --cached --reverse" : "git apply --cached",
              std::move(argv),
              false,
              caps,
              std::move(patch));
  return true;
}

bool unstage_selected_git_entry(ScreenInteractive& screen,
                                ShellTaskController& controller,
                                const WorkspacePersistentState& state,
                                const EnvironmentCapabilities& caps) {
  std::optional<GitStatusEntry> selected;
  {
    std::lock_guard<std::mutex> lock(controller.mutex);
    if (!controller.runtime.git_entries.empty() && controller.runtime.selected_git_index < controller.runtime.git_entries.size()) {
      selected = controller.runtime.git_entries[controller.runtime.selected_git_index];
    }
  }
  if (!selected) {
    set_status(controller, "No git entry selected");
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }
  return run_git_action(screen,
                        controller,
                        state,
                        caps,
                        "git restore --staged",
                        {"git", "-C", state.root.string(), "restore", "--staged", "--", selected->path});
}

bool execute_palette_command(ScreenInteractive& screen,
                             ShellTaskController& controller,
                             WorkspacePersistentState& state,
                             const EnvironmentCapabilities& caps,
                             std::string command_text) {
  auto argv = split_command_line(command_text);
  if (argv.empty()) {
    set_status(controller, "Palette command was empty");
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }

  const auto current_role = [&] {
    std::lock_guard<std::mutex> lock(controller.mutex);
    return state.tabs[controller.runtime.visible_tab].role;
  }();

  const auto command = argv.front();
  if (command == "calc") {
    const auto expression_at = command_text.find_first_not_of(" \t", command.size());
    if (expression_at == std::string::npos) {
      set_status(controller, "Usage: calc <expression>");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    const auto expression = command_text.substr(expression_at);
    double minimum = -10.0;
    double maximum = 10.0;
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      minimum = controller.runtime.math_plot_min;
      maximum = controller.runtime.math_plot_max;
    }
    const auto fields = run_math_worker(state, caps, "calc", expression, minimum, maximum);
    std::vector<std::pair<std::string, std::string>> history;
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      if (fields.size() >= 2 && fields[0] == "ok") {
        controller.runtime.math_expression = expression;
        controller.runtime.math_result = fields.size() >= 3 ? fields[2] : fields[1];
        controller.runtime.math_history.push_back({expression, fields[1]});
        if (controller.runtime.math_history.size() > 100) controller.runtime.math_history.erase(controller.runtime.math_history.begin());
        controller.runtime.status_message = "Math result ready";
        history = controller.runtime.math_history;
      } else {
        controller.runtime.status_message = fields.size() >= 2 ? fields[1] : "Math worker returned invalid output";
      }
    }
    if (!history.empty()) WorkspaceStore{}.save_math_history(state.root, history);
    invalidate_pane_data_snapshot(state.root);
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }
  if (command == "plot-range") {
    if (argv.size() != 3) {
      set_status(controller, "Usage: plot-range <min> <max>");
    } else {
      try {
        const auto minimum = std::stod(argv[1]);
        const auto maximum = std::stod(argv[2]);
        if (minimum >= maximum) throw std::invalid_argument("range");
        std::lock_guard<std::mutex> lock(controller.mutex);
        controller.runtime.math_plot_min = minimum;
        controller.runtime.math_plot_max = maximum;
        controller.runtime.status_message = "Math plot range updated";
      } catch (...) {
        set_status(controller, "Plot range must be two numbers with min < max");
      }
    }
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }
  if (command == "clear-tasks") {
    WorkspaceStore store;
    if (store.clear_tasks(state.root)) {
      std::lock_guard<std::mutex> lock(controller.mutex);
      controller.runtime.task_history.clear();
      controller.runtime.selected_task_index = 0;
      controller.runtime.status_message = "Task history cleared";
    } else {
      set_status(controller, "Failed to clear task history");
    }
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }
  if (command == "editor") {
    if (argv.size() != 2 || (argv[1] != "vim" && argv[1] != "nvim" && argv[1] != "vscode")) {
      set_status(controller, "Usage: editor <vim|nvim|vscode>");
    } else {
      state.external_editor = argv[1];
      set_status(controller,
                 "Editor set to " + editor_display_name(state.external_editor) +
                     (editor_available(state.external_editor, caps) ? "" : " (not found on PATH)"));
    }
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }
  if (command == "run") {
    if (argv.size() > 1) {
      auto command_line = command_text.substr(command_text.find_first_not_of(" \t", command.size()));
      launch_task(screen, controller, state, "palette command", split_command_line(command_line), true, caps);
      return true;
    }
    if (state.recent_commands.empty()) {
      set_status(controller, "No recent command to run");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    launch_task(
        screen, controller, state, "recent command", split_command_line(state.recent_commands.front()), true, caps);
    return true;
  }
  if (command == "rerun") {
    std::vector<std::string> rerun_argv;
    std::string name = "rerun";
    bool use_pty = false;
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      if (!controller.runtime.task_history.empty()) {
        const auto& task = controller.runtime.task_history.back();
        rerun_argv = task.argv;
        name = task.name + " rerun";
        use_pty = task.use_pty;
      }
    }
    if (rerun_argv.empty()) {
      set_status(controller, "No task available to rerun");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    launch_task(screen, controller, state, std::move(name), std::move(rerun_argv), use_pty, caps);
    return true;
  }
  if (command == "rerun-task") {
    std::vector<std::string> rerun_argv;
    std::string name = "rerun";
    bool use_pty = false;
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      if (!controller.runtime.task_history.empty()) {
        const auto index =
            std::min(controller.runtime.selected_task_index, controller.runtime.task_history.size() - 1);
        const auto& task = controller.runtime.task_history[index];
        rerun_argv = task.argv;
        name = task.name + " rerun";
        use_pty = task.use_pty;
      }
    }
    if (rerun_argv.empty()) {
      set_status(controller, "No selected task available to rerun");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    launch_task(screen, controller, state, std::move(name), std::move(rerun_argv), use_pty, caps);
    return true;
  }
  if (command == "search") {
    if (argv.size() < 2) {
      set_status(controller, "Usage: search <query>");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    auto query = command_text.substr(command_text.find_first_not_of(" \t", command.size()));
    launch_search(screen, controller, state, caps, query);
    return true;
  }
  if (command == "git-status") {
    launch_task(screen,
                controller,
                state,
                "git status",
                {"git", "-C", state.root.string(), "status", "--short", "--branch"},
                false,
                caps);
    return true;
  }
  if (command == "git-diff") {
    launch_task(screen,
                controller,
                state,
                "git diff",
                {"git", "-C", state.root.string(), "diff", "--stat", "--compact-summary"},
                false,
                caps);
    return true;
  }
  if (command == "stage") {
    if (argv.size() != 2) {
      set_status(controller, "Usage: stage <path>");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    return run_git_action(
        screen, controller, state, caps, "git add", {"git", "-C", state.root.string(), "add", "--", argv[1]});
  }
  if (command == "unstage") {
    if (argv.size() != 2) {
      set_status(controller, "Usage: unstage <path>");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    return run_git_action(screen,
                          controller,
                          state,
                          caps,
                          "git restore --staged",
                          {"git", "-C", state.root.string(), "restore", "--staged", "--", argv[1]});
  }
  if (command == "stage-hunk") {
    return apply_selected_hunk(screen, controller, state, caps, false);
  }
  if (command == "unstage-hunk") {
    return apply_selected_hunk(screen, controller, state, caps, true);
  }
  if (command == "commit") {
    if (argv.size() < 2) {
      set_status(controller, "Usage: commit <message>");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    auto message = command_text.substr(command_text.find_first_not_of(" \t", command.size()));
    return run_git_action(screen,
                          controller,
                          state,
                          caps,
                          "git commit",
                          {"git", "-C", state.root.string(), "commit", "-m", message});
  }
  if (command == "branch") {
    if (argv.size() != 2) {
      set_status(controller, "Usage: branch <name>");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    return run_git_action(screen,
                          controller,
                          state,
                          caps,
                          "git switch",
                          {"git", "-C", state.root.string(), "switch", "--", argv[1]});
  }
  if (command == "branch-new") {
    if (argv.size() != 2) {
      set_status(controller, "Usage: branch-new <name>");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    return run_git_action(screen,
                          controller,
                          state,
                          caps,
                          "git switch -c",
                          {"git", "-C", state.root.string(), "switch", "-c", argv[1]});
  }
  if (command == "cancel") {
    if (controller.task_running.load()) {
      controller.cancel_requested = true;
      set_status(controller, "Cancellation requested");
    } else {
      set_status(controller, "No active task to cancel");
    }
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }
  if (command == "refresh") {
    if (controller.task_running.load()) {
      controller.cancel_requested = true;
      set_status(controller, "Cancellation requested");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    if (current_role == TabRole::Finance) {
      request_market_quotes(screen, controller, state, caps);
    } else {
      refresh_git_state(controller, state, caps);
      set_status(controller, "Git state refreshed");
      invalidate_pane_data_snapshot(state.root);
      screen.PostEvent(ftxui::Event::Custom);
    }
    return true;
  }
  if (command == "next-file" || command == "prev-file") {
    if (current_role != TabRole::Review) {
      set_status(controller, command + " is only available in Review");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      if (controller.runtime.git_entries.empty()) {
        controller.runtime.status_message = "No git entries available";
      } else if (command == "next-file") {
        controller.runtime.selected_git_index =
            std::min(controller.runtime.selected_git_index + 1, controller.runtime.git_entries.size() - 1);
      } else if (controller.runtime.selected_git_index > 0) {
        --controller.runtime.selected_git_index;
      }
    }
    request_git_diff_refresh(screen, controller, state, caps);
    invalidate_pane_data_snapshot(state.root);
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }
  if (command == "next-task" || command == "prev-task") {
    if (current_role != TabRole::Run) {
      set_status(controller, command + " is only available in Run");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      if (controller.runtime.task_history.empty()) {
        controller.runtime.status_message = "No tasks available";
      } else if (command == "next-task") {
        controller.runtime.selected_task_index =
            std::min(controller.runtime.selected_task_index + 1, controller.runtime.task_history.size() - 1);
      } else if (controller.runtime.selected_task_index > 0) {
        --controller.runtime.selected_task_index;
      }
    }
    invalidate_pane_data_snapshot(state.root);
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }
  if (command == "next-hunk" || command == "prev-hunk") {
    if (current_role != TabRole::Review) {
      set_status(controller, command + " is only available in Review");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      if (controller.runtime.diff_hunks.empty()) {
        controller.runtime.status_message = "No diff hunks available";
      } else if (command == "next-hunk") {
        controller.runtime.selected_diff_hunk =
            std::min(controller.runtime.selected_diff_hunk + 1, controller.runtime.diff_hunks.size() - 1);
      } else if (controller.runtime.selected_diff_hunk > 0) {
        --controller.runtime.selected_diff_hunk;
      }
    }
    invalidate_pane_data_snapshot(state.root);
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }
  if (command == "add") {
    if (current_role != TabRole::Finance) {
      set_status(controller, "add is only available in Finance");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    if (argv.size() != 2) {
      set_status(controller, "Usage: add <ticker>");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    add_watchlist_symbol(screen, controller, state, caps, argv[1]);
    return true;
  }
  if (command == "remove") {
    if (current_role != TabRole::Finance) {
      set_status(controller, "remove is only available in Finance");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    if (argv.size() != 2) {
      set_status(controller, "Usage: remove <ticker>");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    remove_watchlist_symbol(screen, controller, state, argv[1]);
    return true;
  }
  if (command == "alert") {
    if (current_role != TabRole::Finance) {
      set_status(controller, "alert is only available in Finance");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    if (argv.size() < 4) {
      set_status(controller, "Usage: alert <ticker> >= <price> [note]");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    auto rule = trim_copy(command_text.substr(command_text.find_first_not_of(" \t", command.size())));
    add_alert_rule(screen, controller, state, caps, rule);
    return true;
  }
  if (command == "focus") {
    if (current_role != TabRole::Finance) {
      set_status(controller, "focus is only available in Finance");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    if (argv.size() != 2) {
      set_status(controller, "Usage: focus <ticker>");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    std::string symbol = argv[1];
    std::transform(symbol.begin(), symbol.end(), symbol.begin(), [](unsigned char ch) { return std::toupper(ch); });
    bool found = false;
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      for (std::size_t i = 0; i < controller.runtime.market_entries.size(); ++i) {
        if (controller.runtime.market_entries[i].symbol == symbol) {
          controller.runtime.selected_market_index = i;
          controller.runtime.current_market_symbol = symbol;
          controller.runtime.portfolio_lines = portfolio_lines_for(controller.runtime);
          controller.runtime.status_message = "Focused " + symbol;
          found = true;
          break;
        }
      }
    }
    if (!found) {
      set_status(controller, symbol + " is not in the watchlist");
    } else {
      invalidate_pane_data_snapshot(state.root);
    }
    screen.PostEvent(ftxui::Event::Custom);
    return true;
  }
  if (command == "quit") {
    if (controller.task_running.load()) {
      controller.cancel_requested = true;
      controller.exit_requested = true;
    } else {
      screen.ExitLoopClosure()();
    }
    return true;
  }

  set_status(controller, "Unknown palette command: " + command);
  screen.PostEvent(ftxui::Event::Custom);
  return true;
}

Element render_layout_tree(const SplitNode& node,
                           const PaneDataSnapshot& snapshot,
                           const WorkspacePersistentState& state,
                           const WorkspaceRuntimeState& runtime,
                           const EnvironmentCapabilities& caps) {
  if (std::holds_alternative<PaneLeaf>(node.node)) {
    auto pane = make_static_pane(std::get<PaneLeaf>(node.node).kind, snapshot, state, runtime, caps);
    return pane->component()->Render() | flex;
  }
  const auto& branch = std::get<SplitBranch>(node.node);
  auto first = render_layout_tree(*branch.first, snapshot, state, runtime, caps);
  auto second = render_layout_tree(*branch.second, snapshot, state, runtime, caps);
  const int primary = static_cast<int>(branch.ratio * 1000.0);
  const int secondary = 1000 - primary;
  if (branch.axis == SplitAxis::Horizontal) {
    return hbox({
               first | flex | size(WIDTH, EQUAL, primary),
               separator(),
               second | flex | size(WIDTH, EQUAL, secondary),
           }) |
           flex;
  }
  return vbox({
             first | flex | size(HEIGHT, EQUAL, primary),
             separator(),
             second | flex | size(HEIGHT, EQUAL, secondary),
         }) |
         flex;
}

Element render_summary(const WorkspacePersistentState& state,
                       const WorkspaceRuntimeState& runtime,
                       const EnvironmentCapabilities& caps,
                       bool safe_mode) {
  const auto& current = state.tabs[runtime.visible_tab];
  const auto snapshot = build_pane_data_snapshot(state, runtime, caps);
  if (runtime.maximized_pane && contains_pane(current.layout, *runtime.maximized_pane)) {
    auto pane = make_static_pane(*runtime.maximized_pane, snapshot, state, runtime, caps);
    return pane->component()->Render() | flex;
  }
  return render_layout_tree(current.layout, snapshot, state, runtime, caps) | flex;
}

void launch_ftxui_shell(WorkspacePersistentState& state,
                        WorkspaceRuntimeState initial_runtime,
                        const EnvironmentCapabilities& caps,
                        bool safe_mode) {
  ShellTaskController controller;
  controller.runtime = std::move(initial_runtime);
  controller.runtime.visible_tab = state.focused_tab;
  SearchOverlayState search_overlay;
  TickerOverlayState ticker_overlay;
  AlertOverlayState alert_overlay;
  CommandOverlayState command_overlay;
  CommitOverlayState commit_overlay;
  SettingsOverlayState settings_overlay;
  PickerOverlayState picker_overlay;
  // Runtime-backed pane lines are rebuilt from the cached scan on each render. UI-only
  // edits therefore only need a redraw; dropping the cache here made every keystroke
  // rescan the workspace.
  auto invalidate_pane_data_snapshot = [](const std::filesystem::path&) {};

  int tab_index = static_cast<int>(controller.runtime.visible_tab);
  std::vector<std::string> tab_names;
  tab_names.reserve(state.tabs.size());
  for (const auto& tab : state.tabs) {
    tab_names.push_back(tab.name);
  }
  std::vector<std::string> picker_files;
  {
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(
             state.root, std::filesystem::directory_options::skip_permission_denied, ec), end;
         it != end && picker_files.size() < 600; it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;
      }
      const auto relative = it->path().lexically_relative(state.root);
      if (it->is_directory(ec) &&
          (relative.string().starts_with(".git") || relative.string().starts_with("build"))) {
        it.disable_recursion_pending();
        continue;
      }
      if (it->is_regular_file(ec)) {
        picker_files.push_back(relative.string());
      }
    }
  }
  const auto picker_entries = [&] {
    std::vector<TaskRecord> tasks;
    std::size_t visible_tab = 0;
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      tasks = controller.runtime.task_history;
      visible_tab = controller.runtime.visible_tab;
    }
    std::vector<PickerEntry> entries;
    for (const auto& command : palette_suggestions_for(
             state.tabs[visible_tab].role)) {
      const auto action = command.substr(0, command.find(' '));
      entries.push_back({PickerEntryKind::Action, "Action  " + command, {}, 0, std::nullopt, action});
    }
    for (const auto& file : picker_files) {
      entries.push_back({PickerEntryKind::File, "File    " + file, {}, 0, std::nullopt, file});
    }
    for (std::size_t i = 0; i < state.tabs.size(); ++i) {
      entries.push_back({PickerEntryKind::Tab, "Tab     " + state.tabs[i].name, {}, i});
      for (const auto pane : pane_order(state.tabs[i].layout)) {
        entries.push_back(
            {PickerEntryKind::Pane, "Pane    " + state.tabs[i].name + " / " + to_string(pane), {}, i, pane});
      }
    }
    for (const auto& command : state.recent_commands) {
      entries.push_back({PickerEntryKind::RecentCommand,
                         "Recent  " + command,
                         split_command_line(command)});
    }
    for (auto it = tasks.rbegin(); it != tasks.rend(); ++it) {
      if (!it->argv.empty()) {
        entries.push_back({PickerEntryKind::Task, "Task    " + it->name, it->argv});
      }
    }
    for (const auto& recipe : discover_project_recipes(state.root)) {
      entries.push_back({PickerEntryKind::Recipe, "Recipe  " + recipe.label, recipe.argv});
    }
    struct Ranked { int score; std::size_t order; PickerEntry entry; };
    std::vector<Ranked> ranked;
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const auto score = fuzzy_score(picker_overlay.query, entries[i].label);
      if (score >= 0) {
        ranked.push_back({score - static_cast<int>(i / 8), i, std::move(entries[i])});
      }
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
      return left.score > right.score;
    });
    entries.clear();
    for (std::size_t i = 0; i < std::min<std::size_t>(12, ranked.size()); ++i) {
      entries.push_back(std::move(ranked[i].entry));
    }
    return entries;
  };

  auto tabs = Toggle(&tab_names, &tab_index);
  auto header = Renderer([&] {
    WorkspaceRuntimeState runtime_snapshot;
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      runtime_snapshot = controller.runtime;
    }
    const auto& current = state.tabs[runtime_snapshot.visible_tab];
    return hbox({
               text("deck") | bold | color(Color::Cyan),
               text("  " + state.name) | dim,
               text("  /  " + current.name),
               filler(),
               text(editor_display_name(state.external_editor)) | dim,
           }) |
           xflex;
  });
  auto footer = Renderer([&] {
    WorkspaceRuntimeState runtime_snapshot;
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      runtime_snapshot = controller.runtime;
    }
    const auto& current = state.tabs[runtime_snapshot.visible_tab];
    const auto status = runtime_snapshot.status_message.empty() ? std::string("ready")
                                                                  : runtime_snapshot.status_message;
    auto status_element = text(status);
    const auto normalized_status = lower_copy(status);
    if (normalized_status == "ready") {
      status_element = status_element | dim;
    } else if (normalized_status.find("fail") != std::string::npos ||
               normalized_status.find("error") != std::string::npos ||
               normalized_status.find("unavailable") != std::string::npos) {
      status_element = status_element | color(Color::Red);
    } else if (normalized_status.find("missing") != std::string::npos ||
               normalized_status.find("limited") != std::string::npos ||
               normalized_status.find("not found") != std::string::npos) {
      status_element = status_element | color(Color::Yellow);
    } else {
      status_element = status_element | color(Color::Cyan);
    }
    return hbox({
               status_element,
               filler(),
               text(current.name + ": " + controls_for_role(current.role)) | dim,
               text("   ·   , settings   : commands   q quit") | dim,
           }) |
           xflex;
  });

  auto renderer = Renderer(tabs, [&] {
    WorkspaceRuntimeState runtime_snapshot;
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      if (!state.tabs.empty()) {
        controller.runtime.visible_tab =
            static_cast<std::size_t>(std::clamp(tab_index, 0, static_cast<int>(state.tabs.size() - 1)));
        state.focused_tab = controller.runtime.visible_tab;
      }
      runtime_snapshot = controller.runtime;
    }
    if (!state.tabs.empty()) {
      runtime_snapshot.visible_tab =
          static_cast<std::size_t>(std::clamp(tab_index, 0, static_cast<int>(state.tabs.size() - 1)));
    }
    Elements rendered_tabs;
    for (std::size_t i = 0; i < tab_names.size(); ++i) {
      auto tab = text(" " + tab_names[i] + " ");
      if (static_cast<int>(i) == tab_index) {
        tab = tab | bold | underlined | color(Color::Cyan);
      } else {
        tab = tab | dim;
      }
      rendered_tabs.push_back(std::move(tab));
      rendered_tabs.push_back(text(" "));
    }
    auto tab_bar = hbox(std::move(rendered_tabs)) | xflex;
    auto body = render_summary(state, runtime_snapshot, caps, safe_mode);
    Element content = vbox({
               header->Render(),
               separator() | dim,
               tab_bar,
               separator() | dim,
               body,
               separator() | dim,
               footer->Render(),
           }) |
           size(WIDTH, GREATER_THAN, 100) | size(HEIGHT, GREATER_THAN, 28);
    if (search_overlay.active) {
      auto overlay = window(
          text("Search rg"),
          vbox({
              text("Type query and press Enter"),
              separator(),
              text(search_overlay.query.empty() ? std::string(" ") : search_overlay.query),
          }));
      content = dbox({
          content,
          overlay | center,
      });
    }
    if (ticker_overlay.active) {
      auto overlay = window(
          text("Add ticker"),
          vbox({
              text("Type symbol and press Enter"),
              separator(),
              text(ticker_overlay.symbol.empty() ? std::string(" ") : ticker_overlay.symbol),
          }));
      content = dbox({
          content,
          overlay | center,
      });
    }
    if (alert_overlay.active) {
      auto overlay = window(
          text("Add alert"),
          vbox({
              text("Format: NVDA >= 1500 trim position"),
              separator(),
              text(alert_overlay.rule.empty() ? std::string(" ") : alert_overlay.rule),
          }));
      content = dbox({
          content,
          overlay | center,
      });
    }
    if (command_overlay.active) {
      std::vector<std::string> suggestions;
      {
        std::lock_guard<std::mutex> lock(controller.mutex);
        suggestions = controller.runtime.palette_suggestions;
      }
      Elements suggestion_lines;
      for (const auto& suggestion : suggestions) {
        suggestion_lines.push_back(text(suggestion));
      }
      if (suggestion_lines.empty()) {
        suggestion_lines.push_back(text("No commands"));
      }
      auto overlay = window(
          text("Command palette"),
          vbox({
              text("Type a command and press Enter"),
              separator(),
              text(command_overlay.command.empty() ? std::string(" ") : command_overlay.command),
              separator(),
              text("Available") | bold,
              vbox(std::move(suggestion_lines)),
          }));
      content = dbox({
          content,
          overlay | center,
      });
    }
    if (picker_overlay.active) {
      const auto matches = picker_entries();
      Elements rows;
      for (std::size_t i = 0; i < matches.size(); ++i) {
        auto row = text((i == picker_overlay.selected ? "◆ " : "  ") + matches[i].label);
        rows.push_back(i == picker_overlay.selected ? row | color(Color::Cyan) | bold : row | dim);
      }
      if (rows.empty()) {
        rows.push_back(text("  No matches") | dim);
      }
      auto overlay = window(
          text("Quick open  Ctrl-P"),
          vbox({text(picker_overlay.query.empty() ? std::string("Type to filter") : picker_overlay.query),
                separator(), vbox(std::move(rows))}));
      content = dbox({content, overlay | center});
    }
    if (commit_overlay.active) {
      auto overlay = window(
          text("Commit changes"),
          vbox({
              text("Type commit message and press Enter"),
              separator(),
              text(commit_overlay.message.empty() ? std::string(" ") : commit_overlay.message),
          }));
      content = dbox({
          content,
          overlay | center,
      });
    }
    if (settings_overlay.active) {
      const std::vector<std::string> editors = {"vim", "nvim", "vscode"};
      Elements choices;
      for (std::size_t i = 0; i < editors.size(); ++i) {
        const auto& editor = editors[i];
        const auto selected = static_cast<int>(i) == settings_overlay.selected_editor;
        auto label = std::string(selected ? "> " : "  ") + editor_display_name(editor) +
                     (editor_available(editor, caps) ? "  available" : "  missing from PATH");
        choices.push_back(text(label) | (selected ? bold : dim));
      }
      auto overlay = window(
          text("Settings"),
          vbox({
              text("External editor"),
              separator(),
              vbox(std::move(choices)),
              separator(),
              text("Persisted task history: " + std::to_string(runtime_snapshot.task_history.size()) + " / 50") | dim,
              text("c clear task history"),
              separator(),
              text("j/k select   Enter save   Esc close"),
          }));
      content = dbox({
          content,
          overlay | center,
      });
    }
    return content;
  });

  auto screen = ScreenInteractive::FullscreenAlternateScreen();
  // Keyboard controls cover every action. Avoid mouse reporting because it can
  // create a redraw storm in terminals that emit motion events.
  screen.TrackMouse(false);
  if (controller.runtime.market_data_enabled) {
    request_market_quotes(screen, controller, state, caps);
  }
  auto root = CatchEvent(renderer, [&](ftxui::Event event) {
    bool editor_active = false;
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      editor_active = controller.runtime.note_editor.editing || controller.runtime.scratch_editor.editing;
    }
    const bool text_input_active = editor_active || search_overlay.active || ticker_overlay.active ||
                                   alert_overlay.active || command_overlay.active || commit_overlay.active ||
                                   settings_overlay.active || picker_overlay.active;
    if (!text_input_active && event == ftxui::Event::CtrlP) {
      picker_overlay.active = true;
      picker_overlay.query.clear();
      picker_overlay.selected = 0;
      return true;
    }
    if (!text_input_active && (event == ftxui::Event::Tab || event == ftxui::Event::TabReverse)) {
      std::lock_guard<std::mutex> lock(controller.mutex);
      auto& tab = state.tabs[controller.runtime.visible_tab];
      const auto panes = pane_order(tab.layout);
      if (!panes.empty()) {
        const auto found = std::find(panes.begin(), panes.end(), tab.focused_pane);
        const auto current = found == panes.end() ? std::size_t{0}
                                                  : static_cast<std::size_t>(found - panes.begin());
        const auto next = event == ftxui::Event::Tab
                              ? (current + 1) % panes.size()
                              : (current + panes.size() - 1) % panes.size();
        tab.focused_pane = panes[next];
        if (controller.runtime.maximized_pane) {
          controller.runtime.maximized_pane = tab.focused_pane;
        }
        controller.runtime.status_message = "Focus: " + to_string(tab.focused_pane);
      }
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    if (!text_input_active && event == ftxui::Event::CtrlW) {
      std::lock_guard<std::mutex> lock(controller.mutex);
      const auto focused = state.tabs[controller.runtime.visible_tab].focused_pane;
      if (controller.runtime.maximized_pane == focused) {
        controller.runtime.maximized_pane.reset();
        controller.runtime.status_message = "Restored pane layout";
      } else {
        controller.runtime.maximized_pane = focused;
        controller.runtime.status_message = "Maximized " + to_string(focused);
      }
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    if (!text_input_active) {
      const auto input = event.input();
      const bool alt_left = input == "\x1b[1;3D" || input == "\x1b[3D";
      const bool alt_right = input == "\x1b[1;3C" || input == "\x1b[3C";
      const bool alt_up = input == "\x1b[1;3A" || input == "\x1b[3A";
      const bool alt_down = input == "\x1b[1;3B" || input == "\x1b[3B";
      if (alt_left || alt_right || alt_up || alt_down) {
        std::lock_guard<std::mutex> lock(controller.mutex);
        auto& tab = state.tabs[controller.runtime.visible_tab];
        const auto axis = (alt_left || alt_right) ? SplitAxis::Horizontal : SplitAxis::Vertical;
        const double delta = (alt_left || alt_up) ? -0.05 : 0.05;
        if (resize_nearest_split(tab.layout, tab.focused_pane, axis, delta)) {
          controller.runtime.status_message = "Resized " + to_string(tab.focused_pane);
        } else {
          controller.runtime.status_message = "No matching split to resize";
        }
        screen.PostEvent(ftxui::Event::Custom);
        return true;
      }
    }
    if (!text_input_active && event == ftxui::Event::Character('p') &&
        state.tabs[controller.runtime.visible_tab].role == TabRole::Math) {
      std::string expression;
      double minimum = -10.0;
      double maximum = 10.0;
      {
        std::lock_guard<std::mutex> lock(controller.mutex);
        expression = controller.runtime.math_expression;
        minimum = controller.runtime.math_plot_min;
        maximum = controller.runtime.math_plot_max;
      }
      if (expression.empty()) {
        set_status(controller, "Evaluate an expression before plotting");
      } else {
        const auto fields = run_math_worker(state, caps, "plot", expression, minimum, maximum);
        std::lock_guard<std::mutex> lock(controller.mutex);
        if (fields.size() >= 2 && fields[0] == "ok") {
          controller.runtime.math_plot = fields[1];
          controller.runtime.status_message = "Braille plot ready";
        } else {
          controller.runtime.status_message = fields.size() >= 2 ? fields[1] : "Plot failed";
        }
      }
      invalidate_pane_data_snapshot(state.root);
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    if (!text_input_active && event == ftxui::Event::Character('n') &&
        state.tabs[controller.runtime.visible_tab].role == TabRole::Math) {
      std::lock_guard<std::mutex> lock(controller.mutex);
      if (controller.runtime.math_result.empty()) {
        controller.runtime.status_message = "No math result to append";
      } else if (controller.runtime.note_context.kind == NoteContextKind::None) {
        controller.runtime.status_message = "Select a file, search result, or market note first";
      } else {
        if (!controller.runtime.note_editor.buffer.empty()) controller.runtime.note_editor.buffer += "\n\n";
        controller.runtime.note_editor.buffer += "Math: `" + controller.runtime.math_expression + "`\n\n" +
                                                 controller.runtime.math_result;
        controller.runtime.note_editor.cursor = controller.runtime.note_editor.buffer.size();
        controller.runtime.note_editor.dirty = true;
        controller.runtime.status_message = "Math result appended to active note";
      }
      invalidate_pane_data_snapshot(state.root);
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    if (!text_input_active && event == ftxui::Event::Character('r')) {
      if (!state.recent_commands.empty()) {
        auto argv = split_command_line(state.recent_commands.front());
        launch_task(screen, controller, state, "recent command", std::move(argv), true, caps);
      }
      return true;
    }
    if (!text_input_active && event == ftxui::Event::Character('R')) {
      std::vector<std::string> argv;
      std::string name = "rerun";
      bool use_pty = false;
      {
        std::lock_guard<std::mutex> lock(controller.mutex);
        if (!controller.runtime.task_history.empty()) {
          const auto& task = controller.runtime.task_history.back();
          argv = task.argv;
          name = task.name + " rerun";
          use_pty = task.use_pty;
        }
      }
      if (!argv.empty()) {
        launch_task(screen, controller, state, std::move(name), std::move(argv), use_pty, caps);
      }
      return true;
    }
    if (!text_input_active && event == ftxui::Event::Character('x')) {
      if (controller.task_running.load()) {
        controller.cancel_requested = true;
      } else {
        const auto role = [&] {
          std::lock_guard<std::mutex> lock(controller.mutex);
          return state.tabs[controller.runtime.visible_tab].role;
        }();
        if (role == TabRole::Finance) {
          request_market_quotes(screen, controller, state, caps);
        } else {
          refresh_git_state(controller, state, caps);
          set_status(controller, "Git state refreshed");
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
        }
      }
      return true;
    }
    if (search_overlay.active) {
      if (event == ftxui::Event::Escape) {
        search_overlay.active = false;
        search_overlay.query.clear();
        return true;
      }
      if (event == ftxui::Event::Return) {
        search_overlay.active = false;
        launch_search(screen, controller, state, caps, search_overlay.query);
        search_overlay.query.clear();
        return true;
      }
      if (event == ftxui::Event::Backspace) {
        if (!search_overlay.query.empty()) {
          search_overlay.query.pop_back();
        }
        return true;
      }
      if (event.is_character()) {
        search_overlay.query += event.character();
        return true;
      }
    }
    if (ticker_overlay.active) {
      if (event == ftxui::Event::Escape) {
        ticker_overlay.active = false;
        ticker_overlay.symbol.clear();
        return true;
      }
      if (event == ftxui::Event::Return) {
        ticker_overlay.active = false;
        add_watchlist_symbol(screen, controller, state, caps, ticker_overlay.symbol);
        ticker_overlay.symbol.clear();
        return true;
      }
      if (event == ftxui::Event::Backspace) {
        if (!ticker_overlay.symbol.empty()) {
          ticker_overlay.symbol.pop_back();
        }
        return true;
      }
      if (event.is_character()) {
        ticker_overlay.symbol += event.character();
        return true;
      }
    }
    if (alert_overlay.active) {
      if (event == ftxui::Event::Escape) {
        alert_overlay.active = false;
        alert_overlay.rule.clear();
        return true;
      }
      if (event == ftxui::Event::Return) {
        alert_overlay.active = false;
        add_alert_rule(screen, controller, state, caps, alert_overlay.rule);
        alert_overlay.rule.clear();
        return true;
      }
      if (event == ftxui::Event::Backspace) {
        if (!alert_overlay.rule.empty()) {
          alert_overlay.rule.pop_back();
        }
        return true;
      }
      if (event.is_character()) {
        alert_overlay.rule += event.character();
        return true;
      }
    }
    if (picker_overlay.active) {
      auto matches = picker_entries();
      if (event == ftxui::Event::Escape || event == ftxui::Event::CtrlP) {
        picker_overlay.active = false;
        return true;
      }
      if (event == ftxui::Event::ArrowDown || event == ftxui::Event::CtrlJ) {
        if (!matches.empty()) {
          picker_overlay.selected = std::min(picker_overlay.selected + 1, matches.size() - 1);
        }
        return true;
      }
      if (event == ftxui::Event::ArrowUp || event == ftxui::Event::CtrlK) {
        if (picker_overlay.selected > 0) {
          --picker_overlay.selected;
        }
        return true;
      }
      if (event == ftxui::Event::Backspace) {
        if (!picker_overlay.query.empty()) {
          picker_overlay.query.pop_back();
          picker_overlay.selected = 0;
        }
        return true;
      }
      if (event.is_character()) {
        picker_overlay.query += event.character();
        picker_overlay.selected = 0;
        return true;
      }
      if (event == ftxui::Event::Return && !matches.empty()) {
        const auto selected = matches[std::min(picker_overlay.selected, matches.size() - 1)];
        picker_overlay.active = false;
        if (selected.kind == PickerEntryKind::Tab || selected.kind == PickerEntryKind::Pane) {
          tab_index = static_cast<int>(selected.tab_index);
          std::lock_guard<std::mutex> lock(controller.mutex);
          controller.runtime.visible_tab = selected.tab_index;
          state.focused_tab = selected.tab_index;
          if (selected.pane) {
            state.tabs[selected.tab_index].focused_pane = *selected.pane;
          }
          controller.runtime.status_message = "Opened " + selected.label;
          return true;
        }
        if (selected.kind == PickerEntryKind::File) {
          const SearchResult result{selected.value, 1, 1, {}};
          if (!open_in_configured_editor(screen, state, caps, result)) {
            set_status(controller, editor_display_name(state.external_editor) + " is not available on PATH");
          }
          return true;
        }
        if (selected.kind == PickerEntryKind::Action) {
          if (selected.value == "calc") {
            command_overlay.active = true;
            command_overlay.command = "calc ";
            return true;
          }
          return execute_palette_command(screen, controller, state, caps, selected.value);
        }
        launch_task(screen, controller, state, selected.label, selected.argv, true, caps);
        return true;
      }
      return true;
    }
    if (command_overlay.active) {
      if (event == ftxui::Event::Escape) {
        command_overlay.active = false;
        command_overlay.command.clear();
        return true;
      }
      if (event == ftxui::Event::Return) {
        command_overlay.active = false;
        auto command = command_overlay.command;
        command_overlay.command.clear();
        return execute_palette_command(screen, controller, state, caps, std::move(command));
      }
      if (event == ftxui::Event::Backspace) {
        if (!command_overlay.command.empty()) {
          command_overlay.command.pop_back();
        }
        return true;
      }
      if (event.is_character()) {
        command_overlay.command += event.character();
        return true;
      }
    }
    if (commit_overlay.active) {
      if (event == ftxui::Event::Escape) {
        commit_overlay.active = false;
        commit_overlay.message.clear();
        return true;
      }
      if (event == ftxui::Event::Return) {
        commit_overlay.active = false;
        auto message = commit_overlay.message;
        commit_overlay.message.clear();
        if (message.empty()) {
          set_status(controller, "Commit message was empty");
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        return run_git_action(screen,
                              controller,
                              state,
                              caps,
                              "git commit",
                              {"git", "-C", state.root.string(), "commit", "-m", message});
      }
      if (event == ftxui::Event::Backspace) {
        if (!commit_overlay.message.empty()) {
          commit_overlay.message.pop_back();
        }
        return true;
      }
      if (event.is_character()) {
        commit_overlay.message += event.character();
        return true;
      }
    }
    if (settings_overlay.active) {
      if (event == ftxui::Event::Escape) {
        settings_overlay.active = false;
        return true;
      }
      if (event == ftxui::Event::Character('j') || event == ftxui::Event::ArrowDown) {
        settings_overlay.selected_editor = std::min(settings_overlay.selected_editor + 1, 2);
        return true;
      }
      if (event == ftxui::Event::Character('k') || event == ftxui::Event::ArrowUp) {
        settings_overlay.selected_editor = std::max(settings_overlay.selected_editor - 1, 0);
        return true;
      }
      if (event == ftxui::Event::Character('c')) {
        WorkspaceStore store;
        if (store.clear_tasks(state.root)) {
          std::lock_guard<std::mutex> lock(controller.mutex);
          controller.runtime.task_history.clear();
          controller.runtime.selected_task_index = 0;
          controller.runtime.status_message = "Task history cleared";
        } else {
          set_status(controller, "Failed to clear task history");
        }
        screen.PostEvent(ftxui::Event::Custom);
        return true;
      }
      if (event == ftxui::Event::Return) {
        static const std::vector<std::string> editors = {"vim", "nvim", "vscode"};
        state.external_editor = editors[settings_overlay.selected_editor];
        settings_overlay.active = false;
        set_status(controller,
                   "Editor set to " + editor_display_name(state.external_editor) +
                       (editor_available(state.external_editor, caps) ? "" : " (not found on PATH)"));
        screen.PostEvent(ftxui::Event::Custom);
        return true;
      }
      return true;
    }
    if (controller.runtime.note_editor.editing) {
      if (event == ftxui::Event::Escape) {
        std::lock_guard<std::mutex> lock(controller.mutex);
        controller.runtime.note_editor.editing = false;
        invalidate_pane_data_snapshot(state.root);
        screen.PostEvent(ftxui::Event::Custom);
        return true;
      }
      if (event == ftxui::Event::CtrlS) {
        std::string text;
        NoteContext context;
        {
          std::lock_guard<std::mutex> lock(controller.mutex);
          text = controller.runtime.note_editor.buffer;
          context = controller.runtime.note_context;
        }
        if (save_context_note(state.root, context, text)) {
          std::lock_guard<std::mutex> lock(controller.mutex);
          controller.runtime.note_editor.dirty = false;
          controller.runtime.status_message = "Note saved";
        } else {
          set_status(controller, "Failed to save note");
        }
        invalidate_pane_data_snapshot(state.root);
        screen.PostEvent(ftxui::Event::Custom);
        return true;
      }
      if (event == ftxui::Event::CtrlR) {
        std::lock_guard<std::mutex> lock(controller.mutex);
        controller.runtime.note_editor.buffer = load_context_note(state.root, controller.runtime.note_context);
        controller.runtime.note_editor.cursor = controller.runtime.note_editor.buffer.size();
        controller.runtime.note_editor.dirty = false;
        controller.runtime.status_message = "Note reloaded";
        invalidate_pane_data_snapshot(state.root);
        screen.PostEvent(ftxui::Event::Custom);
        return true;
      }
      {
        std::lock_guard<std::mutex> lock(controller.mutex);
        auto& editor = controller.runtime.note_editor;
        auto& buffer = editor.buffer;
        auto& cursor = editor.cursor;
        if (event == ftxui::Event::ArrowLeft) {
          if (cursor > 0) {
            --cursor;
          }
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::ArrowRight) {
          if (cursor < buffer.size()) {
            ++cursor;
          }
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::ArrowUp) {
          cursor = move_cursor_vertical(buffer, cursor, -1);
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::ArrowDown) {
          cursor = move_cursor_vertical(buffer, cursor, 1);
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::Home) {
          cursor = line_start_for(buffer, cursor);
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::End) {
          cursor = line_end_for(buffer, cursor);
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::Backspace) {
          if (cursor > 0) {
            buffer.erase(cursor - 1, 1);
            --cursor;
            editor.dirty = true;
          }
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::Delete) {
          if (cursor < buffer.size()) {
            buffer.erase(cursor, 1);
            editor.dirty = true;
          }
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::Return) {
          buffer.insert(cursor, "\n");
          ++cursor;
          editor.dirty = true;
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::Tab) {
          buffer.insert(cursor, "  ");
          cursor += 2;
          editor.dirty = true;
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event.is_character()) {
          buffer.insert(cursor, event.character());
          cursor += event.character().size();
          editor.dirty = true;
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
      }
    }
    if (state.tabs[controller.runtime.visible_tab].role == TabRole::Notes &&
        controller.runtime.scratch_editor.editing) {
      if (event == ftxui::Event::Escape) {
        std::lock_guard<std::mutex> lock(controller.mutex);
        controller.runtime.scratch_editor.editing = false;
        invalidate_pane_data_snapshot(state.root);
        screen.PostEvent(ftxui::Event::Custom);
        return true;
      }
      if (event == ftxui::Event::CtrlS) {
        std::string text;
        {
          std::lock_guard<std::mutex> lock(controller.mutex);
          text = controller.runtime.scratch_editor.buffer;
        }
        if (save_scratch_buffer(state.root, text)) {
          std::lock_guard<std::mutex> lock(controller.mutex);
          controller.runtime.scratch_editor.dirty = false;
          controller.runtime.status_message = "Scratch saved";
        } else {
          set_status(controller, "Failed to save scratch");
        }
        invalidate_pane_data_snapshot(state.root);
        screen.PostEvent(ftxui::Event::Custom);
        return true;
      }
      if (event == ftxui::Event::CtrlR) {
        std::lock_guard<std::mutex> lock(controller.mutex);
        controller.runtime.scratch_editor.buffer = load_scratch_buffer(state.root);
        controller.runtime.scratch_editor.cursor = controller.runtime.scratch_editor.buffer.size();
        controller.runtime.scratch_editor.dirty = false;
        controller.runtime.status_message = "Scratch reloaded";
        invalidate_pane_data_snapshot(state.root);
        screen.PostEvent(ftxui::Event::Custom);
        return true;
      }
      {
        std::lock_guard<std::mutex> lock(controller.mutex);
        auto& editor = controller.runtime.scratch_editor;
        auto& buffer = editor.buffer;
        auto& cursor = editor.cursor;
        if (event == ftxui::Event::ArrowLeft) {
          if (cursor > 0) {
            --cursor;
          }
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::ArrowRight) {
          if (cursor < buffer.size()) {
            ++cursor;
          }
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::ArrowUp) {
          cursor = move_cursor_vertical(buffer, cursor, -1);
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::ArrowDown) {
          cursor = move_cursor_vertical(buffer, cursor, 1);
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::Home) {
          cursor = line_start_for(buffer, cursor);
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::End) {
          cursor = line_end_for(buffer, cursor);
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::Backspace) {
          if (cursor > 0) {
            buffer.erase(cursor - 1, 1);
            --cursor;
            editor.dirty = true;
          }
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::Delete) {
          if (cursor < buffer.size()) {
            buffer.erase(cursor, 1);
            editor.dirty = true;
          }
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::Return) {
          buffer.insert(cursor, "\n");
          ++cursor;
          editor.dirty = true;
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event == ftxui::Event::Tab) {
          buffer.insert(cursor, "  ");
          cursor += 2;
          editor.dirty = true;
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        if (event.is_character()) {
          buffer.insert(cursor, event.character());
          cursor += event.character().size();
          editor.dirty = true;
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
      }
    }
    if (event == ftxui::Event::Character(':')) {
      command_overlay.active = true;
      command_overlay.command.clear();
      {
        std::lock_guard<std::mutex> lock(controller.mutex);
        controller.runtime.palette_suggestions =
            palette_suggestions_for(state.tabs[controller.runtime.visible_tab].role);
      }
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    if (event == ftxui::Event::Character(',')) {
      settings_overlay.active = true;
      settings_overlay.selected_editor = state.external_editor == "vim" ? 0 : state.external_editor == "vscode" ? 2 : 1;
      return true;
    }
    if (event == ftxui::Event::Character('/')) {
      search_overlay.active = true;
      search_overlay.query.clear();
      return true;
    }
    if (event == ftxui::Event::Character('a')) {
      const auto role = [&] {
        std::lock_guard<std::mutex> lock(controller.mutex);
        return state.tabs[controller.runtime.visible_tab].role;
      }();
      if (role == TabRole::Finance) {
        ticker_overlay.active = true;
        ticker_overlay.symbol.clear();
        return true;
      }
    }
    if (event == ftxui::Event::Character('A')) {
      const auto role = [&] {
        std::lock_guard<std::mutex> lock(controller.mutex);
        return state.tabs[controller.runtime.visible_tab].role;
      }();
      if (role == TabRole::Finance) {
        alert_overlay.active = true;
        alert_overlay.rule.clear();
        return true;
      }
    }
    if (event == ftxui::Event::Character('e')) {
      const auto role = [&] {
        std::lock_guard<std::mutex> lock(controller.mutex);
        return state.tabs[controller.runtime.visible_tab].role;
      }();
      if (role == TabRole::Finance) {
        std::lock_guard<std::mutex> lock(controller.mutex);
        refresh_note_context(state.root, controller.runtime);
        if (controller.runtime.note_context.kind == NoteContextKind::None) {
          controller.runtime.status_message = "No note context available";
        } else {
          controller.runtime.note_editor.editing = true;
          controller.runtime.note_editor.cursor =
              std::min(controller.runtime.note_editor.cursor, controller.runtime.note_editor.buffer.size());
          controller.runtime.status_message = "Context note editing";
        }
        invalidate_pane_data_snapshot(state.root);
        screen.PostEvent(ftxui::Event::Custom);
        return true;
      }
      if (role == TabRole::Notes) {
        std::lock_guard<std::mutex> lock(controller.mutex);
        controller.runtime.scratch_editor.editing = true;
        controller.runtime.scratch_editor.cursor =
            std::min(controller.runtime.scratch_editor.cursor, controller.runtime.scratch_editor.buffer.size());
        controller.runtime.status_message = "Scratch editing";
        invalidate_pane_data_snapshot(state.root);
        screen.PostEvent(ftxui::Event::Custom);
        return true;
      }
    }
    if (event == ftxui::Event::Character('E')) {
      std::lock_guard<std::mutex> lock(controller.mutex);
      refresh_note_context(state.root, controller.runtime);
      if (controller.runtime.note_context.kind == NoteContextKind::None) {
        controller.runtime.status_message = "No note context available";
      } else {
        controller.runtime.note_editor.editing = true;
        controller.runtime.note_editor.cursor =
            std::min(controller.runtime.note_editor.cursor, controller.runtime.note_editor.buffer.size());
        controller.runtime.status_message = "Context note editing";
      }
      invalidate_pane_data_snapshot(state.root);
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    if (event == ftxui::Event::Character('f')) {
      const auto role = [&] {
        std::lock_guard<std::mutex> lock(controller.mutex);
        return state.tabs[controller.runtime.visible_tab].role;
      }();
      if (role == TabRole::Review) {
        std::lock_guard<std::mutex> lock(controller.mutex);
        controller.runtime.review_files_mode = !controller.runtime.review_files_mode;
        controller.runtime.status_message = controller.runtime.review_files_mode
                                                ? "Review Files mode: j/k select, Enter open or browse"
                                                : "Review Changes mode: j/k select changed file";
        screen.PostEvent(ftxui::Event::Custom);
        return true;
      }
    }
    if (event == ftxui::Event::Character('s')) {
      const auto role = [&] {
        std::lock_guard<std::mutex> lock(controller.mutex);
        return state.tabs[controller.runtime.visible_tab].role;
      }();
      if (role == TabRole::Review) {
        return stage_selected_git_entry(screen, controller, state, caps);
      }
    }
    if (event == ftxui::Event::Character('u')) {
      const auto role = [&] {
        std::lock_guard<std::mutex> lock(controller.mutex);
        return state.tabs[controller.runtime.visible_tab].role;
      }();
      if (role == TabRole::Review) {
        return unstage_selected_git_entry(screen, controller, state, caps);
      }
    }
    if (event == ftxui::Event::Character('S')) {
      const auto role = [&] {
        std::lock_guard<std::mutex> lock(controller.mutex);
        return state.tabs[controller.runtime.visible_tab].role;
      }();
      if (role == TabRole::Review) {
        return apply_selected_hunk(screen, controller, state, caps, false);
      }
    }
    if (event == ftxui::Event::Character('U')) {
      const auto role = [&] {
        std::lock_guard<std::mutex> lock(controller.mutex);
        return state.tabs[controller.runtime.visible_tab].role;
      }();
      if (role == TabRole::Review) {
        return apply_selected_hunk(screen, controller, state, caps, true);
      }
    }
    if (event == ftxui::Event::Character('c')) {
      const auto role = [&] {
        std::lock_guard<std::mutex> lock(controller.mutex);
        return state.tabs[controller.runtime.visible_tab].role;
      }();
      if (role == TabRole::Review) {
        commit_overlay.active = true;
        commit_overlay.message.clear();
        return true;
      }
    }
    if (event == ftxui::Event::Character('j')) {
      bool review_changed = false;
      {
        std::lock_guard<std::mutex> lock(controller.mutex);
        const auto role = state.tabs[controller.runtime.visible_tab].role;
        if (role == TabRole::Finance) {
          if (!controller.runtime.market_entries.empty()) {
            controller.runtime.selected_market_index =
                std::min(controller.runtime.selected_market_index + 1, controller.runtime.market_entries.size() - 1);
            refresh_note_context(state.root, controller.runtime);
          }
        } else if (role == TabRole::Review && controller.runtime.review_files_mode) {
          if (!controller.runtime.files_entries.empty()) {
            controller.runtime.selected_file_index =
                std::min(controller.runtime.selected_file_index + 1, controller.runtime.files_entries.size() - 1);
            refresh_note_context(state.root, controller.runtime);
          }
        } else if (role == TabRole::Review) {
          if (!controller.runtime.git_entries.empty()) {
            const auto before = controller.runtime.selected_git_index;
            controller.runtime.selected_git_index =
                std::min(controller.runtime.selected_git_index + 1, controller.runtime.git_entries.size() - 1);
            review_changed = controller.runtime.selected_git_index != before;
          }
        } else if (role == TabRole::Run) {
          if (!controller.runtime.task_history.empty()) {
            controller.runtime.selected_task_index =
                std::min(controller.runtime.selected_task_index + 1, controller.runtime.task_history.size() - 1);
          }
        } else if (!controller.runtime.files_entries.empty()) {
          controller.runtime.selected_file_index =
              std::min(controller.runtime.selected_file_index + 1, controller.runtime.files_entries.size() - 1);
          refresh_note_context(state.root, controller.runtime);
        }
      }
      if (review_changed) {
        request_git_diff_refresh(screen, controller, state, caps);
        invalidate_pane_data_snapshot(state.root);
        screen.PostEvent(ftxui::Event::Custom);
      }
      return true;
    }
    if (event == ftxui::Event::Character('k')) {
      bool review_changed = false;
      {
        std::lock_guard<std::mutex> lock(controller.mutex);
        const auto role = state.tabs[controller.runtime.visible_tab].role;
        if (role == TabRole::Finance) {
          if (controller.runtime.selected_market_index > 0) {
            --controller.runtime.selected_market_index;
            refresh_note_context(state.root, controller.runtime);
          }
        } else if (role == TabRole::Review && controller.runtime.review_files_mode) {
          if (controller.runtime.selected_file_index > 0) {
            --controller.runtime.selected_file_index;
            refresh_note_context(state.root, controller.runtime);
          }
        } else if (role == TabRole::Review) {
          if (controller.runtime.selected_git_index > 0) {
            --controller.runtime.selected_git_index;
            review_changed = true;
          }
        } else if (role == TabRole::Run) {
          if (controller.runtime.selected_task_index > 0) {
            --controller.runtime.selected_task_index;
          }
        } else if (controller.runtime.selected_file_index > 0) {
          --controller.runtime.selected_file_index;
          refresh_note_context(state.root, controller.runtime);
        }
      }
      if (review_changed) {
        request_git_diff_refresh(screen, controller, state, caps);
        invalidate_pane_data_snapshot(state.root);
        screen.PostEvent(ftxui::Event::Custom);
      }
      return true;
    }
    if (event == ftxui::Event::Character(']')) {
      bool handled_review = false;
      {
        std::lock_guard<std::mutex> lock(controller.mutex);
        const auto role = state.tabs[controller.runtime.visible_tab].role;
        if (role == TabRole::Review) {
          handled_review = true;
          if (!controller.runtime.diff_hunks.empty()) {
            controller.runtime.selected_diff_hunk =
                std::min(controller.runtime.selected_diff_hunk + 1, controller.runtime.diff_hunks.size() - 1);
          }
        } else if (!controller.runtime.search_results.empty()) {
          controller.runtime.selected_search_result =
              std::min(controller.runtime.selected_search_result + 1, controller.runtime.search_results.size() - 1);
          refresh_note_context(state.root, controller.runtime);
        }
      }
      if (handled_review) {
        invalidate_pane_data_snapshot(state.root);
        screen.PostEvent(ftxui::Event::Custom);
      }
      return true;
    }
    if (event == ftxui::Event::Character('[')) {
      bool handled_review = false;
      {
        std::lock_guard<std::mutex> lock(controller.mutex);
        const auto role = state.tabs[controller.runtime.visible_tab].role;
        if (role == TabRole::Review) {
          handled_review = true;
          if (controller.runtime.selected_diff_hunk > 0) {
            --controller.runtime.selected_diff_hunk;
          }
        } else if (controller.runtime.selected_search_result > 0) {
          --controller.runtime.selected_search_result;
          refresh_note_context(state.root, controller.runtime);
        }
      }
      if (handled_review) {
        invalidate_pane_data_snapshot(state.root);
        screen.PostEvent(ftxui::Event::Custom);
      }
      return true;
    }
    if (event == ftxui::Event::Return) {
      const auto role = [&] {
        std::lock_guard<std::mutex> lock(controller.mutex);
        return state.tabs[controller.runtime.visible_tab].role;
      }();
      if (role == TabRole::Finance) {
        std::optional<MarketEntry> selected_market;
        {
          std::lock_guard<std::mutex> lock(controller.mutex);
          if (!controller.runtime.market_entries.empty() &&
              controller.runtime.selected_market_index < controller.runtime.market_entries.size()) {
            selected_market = controller.runtime.market_entries[controller.runtime.selected_market_index];
            controller.runtime.current_market_symbol = selected_market->symbol;
            controller.runtime.portfolio_lines = portfolio_lines_for(controller.runtime);
            refresh_note_context(state.root, controller.runtime);
            controller.runtime.status_message = "Focused " + selected_market->symbol;
          }
        }
        if (selected_market) {
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          request_market_quotes(screen, controller, state, caps);
          return true;
        }
      }
      bool review_files_mode = false;
      if (role == TabRole::Review) {
        std::optional<GitStatusEntry> selected_git;
        int target_line = 1;
        {
          std::lock_guard<std::mutex> lock(controller.mutex);
          review_files_mode = controller.runtime.review_files_mode;
          if (!controller.runtime.git_entries.empty() &&
              controller.runtime.selected_git_index < controller.runtime.git_entries.size()) {
            selected_git = controller.runtime.git_entries[controller.runtime.selected_git_index];
          }
          if (!controller.runtime.diff_hunks.empty() &&
              controller.runtime.selected_diff_hunk < controller.runtime.diff_hunks.size()) {
            target_line = std::max(controller.runtime.diff_hunks[controller.runtime.selected_diff_hunk].new_start, 1);
          }
        }
        if (!review_files_mode && selected_git) {
          SearchResult file_result;
          file_result.path = selected_git->path;
          file_result.line = target_line;
          if (open_in_configured_editor(screen, state, caps, file_result)) {
            set_status(controller, "Opened " + selected_git->path);
          } else {
            set_status(controller, editor_display_name(state.external_editor) + " is not available on PATH");
          }
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
      }
      if (role == TabRole::Run) {
        std::vector<std::string> rerun_argv;
        std::string name = "rerun";
        bool use_pty = false;
        {
          std::lock_guard<std::mutex> lock(controller.mutex);
          if (!controller.runtime.task_history.empty()) {
            const auto index =
                std::min(controller.runtime.selected_task_index, controller.runtime.task_history.size() - 1);
            const auto& task = controller.runtime.task_history[index];
            rerun_argv = task.argv;
            name = task.name + " rerun";
            use_pty = task.use_pty;
          }
        }
        if (!rerun_argv.empty()) {
          launch_task(screen, controller, state, std::move(name), std::move(rerun_argv), use_pty, caps);
          return true;
        }
      }
      std::optional<SearchResult> selected;
      {
        std::lock_guard<std::mutex> lock(controller.mutex);
        if (!controller.runtime.search_results.empty() &&
            controller.runtime.selected_search_result < controller.runtime.search_results.size()) {
          selected = controller.runtime.search_results[controller.runtime.selected_search_result];
          refresh_note_context(state.root, controller.runtime);
        }
      }
      if (selected) {
        if (open_in_configured_editor(screen, state, caps, *selected)) {
          set_status(controller, "Opened search result");
        } else {
          set_status(controller, editor_display_name(state.external_editor) + " is not available on PATH");
        }
        invalidate_pane_data_snapshot(state.root);
        screen.PostEvent(ftxui::Event::Custom);
        return true;
      }
      std::optional<FileEntry> file_entry;
      std::string files_browser_root;
      {
        std::lock_guard<std::mutex> lock(controller.mutex);
        if (!controller.runtime.files_entries.empty() &&
            controller.runtime.selected_file_index < controller.runtime.files_entries.size()) {
          file_entry = controller.runtime.files_entries[controller.runtime.selected_file_index];
          files_browser_root = controller.runtime.files_browser_root;
          refresh_note_context(state.root, controller.runtime);
        }
      }
      if (file_entry) {
        if (file_entry->is_directory) {
          std::lock_guard<std::mutex> lock(controller.mutex);
          std::filesystem::path next_root = controller.runtime.files_browser_root;
          if (file_entry->path == "..") {
            next_root = std::filesystem::path(controller.runtime.files_browser_root).parent_path();
            if (next_root.empty()) {
              next_root = ".";
            }
          } else {
            next_root /= file_entry->path;
          }
          controller.runtime.files_browser_root = next_root.lexically_normal().string();
          if (controller.runtime.files_browser_root.empty()) {
            controller.runtime.files_browser_root = ".";
          }
          controller.runtime.files_entries = discover_file_entries(state.root, controller.runtime.files_browser_root);
          controller.runtime.selected_file_index = 0;
          controller.runtime.status_message = "Browsing " + controller.runtime.files_browser_root;
          refresh_note_context(state.root, controller.runtime);
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
          return true;
        }
        SearchResult file_result;
        const auto relative_file = (std::filesystem::path(files_browser_root) / file_entry->path).lexically_normal();
        file_result.path = relative_file.string();
        file_result.line = 1;
        if (open_in_configured_editor(screen, state, caps, file_result)) {
          set_status(controller, "Opened " + file_entry->path);
        } else {
          set_status(controller, editor_display_name(state.external_editor) + " is not available on PATH");
        }
        invalidate_pane_data_snapshot(state.root);
        screen.PostEvent(ftxui::Event::Custom);
        return true;
      }
    }
    if (event == ftxui::Event::Character('q') || event == ftxui::Event::Escape) {
      if (controller.task_running.load()) {
        controller.cancel_requested = true;
        controller.exit_requested = true;
      } else {
        screen.ExitLoopClosure()();
      }
      return true;
    }
    if (event == ftxui::Event::Custom) {
      return true;
    }
    return false;
  });

  screen.Loop(root);
  std::cout << screen.ResetPosition(true) << "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l" << std::flush;
  controller.cancel_requested = true;
  if (controller.worker.joinable()) {
    controller.worker.join();
  }
  if (controller.search_worker.joinable()) {
    controller.search_worker.join();
  }
  if (controller.market_worker.joinable()) {
    controller.market_worker.request_stop();
    controller.market_worker.join();
  }
  if (controller.git_diff_worker.joinable()) {
    controller.git_diff_worker.join();
  }
}

std::filesystem::path normalize_workspace_path(const CliOptions& options) {
  if (options.workspace_open || options.workspace_reset_layout) {
    return std::filesystem::absolute(options.workspace_path);
  }
  return std::filesystem::current_path();
}

void render_safe_summary(const WorkspacePersistentState& persistent,
                         const WorkspaceRuntimeState& runtime,
                         const EnvironmentCapabilities& caps) {
  std::cout << "deck safe mode (read-only summary)\n";
  std::cout << "workspace: " << persistent.root << "\n";
  std::cout << "tabs: " << persistent.tabs.size() << "\n";
  std::cout << "files shown: " << runtime.files_entries.size() << "\n";
  std::cout << "watchlist: " << runtime.market_entries.size() << " symbols\n";
  std::cout << "candles: " << runtime.market_candles.size() << " symbols with OHLC history\n";
  std::cout << "git: " << (caps.git ? "available" : "missing") << "\n";
  std::cout << "search: " << (caps.rg ? "available" : "missing") << "\n";
  std::cout << "market data: " << (runtime.market_data_enabled ? runtime.market_data_provider : "disabled") << "\n";
  std::cout << "status: " << (runtime.status_message.empty() ? "ready" : runtime.status_message) << "\n";
  std::cout << "Run `deck` without --safe to open the interactive workspace.\n";
}

}  // namespace

int run_app(const CliOptions& options) {
  const auto root = normalize_workspace_path(options);
  const auto caps = detect_environment(root);

  if (options.doctor) {
    for (const auto& line : render_doctor_report(caps)) {
      std::cout << line << "\n";
    }
    return 0;
  }
  WorkspaceStore store;

  if (options.workspace_list) {
    std::cout << root << "\n";
    return 0;
  }

  if (options.workspace_reset_layout) {
    if (!store.reset_layout(root)) {
      std::cerr << "failed to reset layout for " << root << "\n";
      return 1;
    }
    std::cout << "reset layout for " << root << "\n";
    return 0;
  }

  WorkspacePersistentState persistent = make_default_workspace(root);
  if (!options.safe_mode) {
    if (auto loaded = store.load(root)) {
      persistent = *loaded;
    }
    ensure_workspace_tabs(persistent);
  }

  WorkspaceRuntimeState runtime;
  runtime.overlays_enabled = !options.safe_mode;
  bootstrap_runtime_state(persistent, caps, options.safe_mode, runtime);
  if (!options.safe_mode) {
    runtime.task_history = store.load_tasks(root);
    runtime.selected_task_index = runtime.task_history.empty() ? 0 : runtime.task_history.size() - 1;
    runtime.math_history = store.load_math_history(root);
    if (!runtime.math_history.empty()) {
      runtime.math_expression = runtime.math_history.back().first;
      runtime.math_result = runtime.math_history.back().second;
    }
  }

  if (options.safe_mode) {
    render_safe_summary(persistent, runtime, caps);
    return 0;
  }

  if (options.workspace_open || std::filesystem::is_directory(root)) {
    launch_ftxui_shell(persistent, runtime, caps, options.safe_mode);
  } else {
    std::cout << "workspace: " << persistent.name << "\n";
  }

  if (!options.safe_mode) {
    store.save(persistent);
  }

  return 0;
}

}  // namespace deck
