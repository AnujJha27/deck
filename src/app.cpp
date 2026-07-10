#include "deck/app.h"

#include "deck/environment.h"
#include "deck/event_bus.h"
#include "deck/panes.h"
#include "deck/persistence.h"
#include "deck/process.h"
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
  return {NoteContextKind::File, entry.path, entry.path};
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

void open_in_nvim(ScreenInteractive& screen, const WorkspacePersistentState& state, const SearchResult& result) {
  ProcessRunner runner;
  ProcessRequest request;
  request.cwd = state.root;
  const auto full_path = std::filesystem::absolute(state.root / result.path);
  request.argv = {"nvim", "+" + std::to_string(std::max(result.line, 1)), full_path.string()};
  auto attached = screen.WithRestoredIO([&] { runner.run_attached(request); });
  attached();
}

void append_tail(std::string& target, const std::string& chunk, std::size_t limit = 400) {
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

TaskRecord make_task_record(const std::string& name, const std::vector<std::string>& argv) {
  TaskRecord record;
  record.name = name;
  record.argv = argv;
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
};

struct SearchOverlayState {
  bool active = false;
  std::string query;
};

struct TickerOverlayState {
  bool active = false;
  std::string symbol;
};

struct CommandOverlayState {
  bool active = false;
  std::string command;
};

struct CommitOverlayState {
  bool active = false;
  std::string message;
};

void set_status(ShellTaskController& controller, const std::string& message);

std::vector<FileEntry> discover_file_entries(const std::filesystem::path& root) {
  std::vector<FileEntry> entries;
  std::error_code ec;
  std::filesystem::recursive_directory_iterator it(
      root, std::filesystem::directory_options::skip_permission_denied, ec);
  const auto end = std::filesystem::recursive_directory_iterator();
  while (!ec && it != end) {
    const auto path = it->path();
    const auto name = path.filename().string();
    if (it->is_directory(ec) && (name == ".git" || name == ".deck" || name == "build")) {
      it.disable_recursion_pending();
      ++it;
      continue;
    }
    if (it->is_regular_file(ec)) {
      auto relative = std::filesystem::relative(path, root, ec);
      if (!ec) {
        entries.push_back(FileEntry{relative.string(), false});
      }
    }
    ++it;
  }
  std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) { return lhs.path < rhs.path; });
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
    if (ch == ',' && !in_quotes) {
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

std::vector<AlertRule> load_alert_rules(const std::filesystem::path& root) {
  std::vector<AlertRule> rules;
  std::ifstream in(root / ".deck" / "alerts.txt");
  if (!in) {
    return rules;
  }
  std::string line;
  while (std::getline(in, line)) {
    auto cleaned = trim_copy(line);
    if (cleaned.empty() || cleaned[0] == '#') {
      continue;
    }

    std::istringstream row(cleaned);
    std::string symbol;
    std::string op;
    std::string threshold_token;
    if (!(row >> symbol >> op >> threshold_token)) {
      continue;
    }
    auto threshold = parse_decimal(threshold_token);
    if (!threshold) {
      continue;
    }

    AlertRule rule;
    std::transform(symbol.begin(),
                   symbol.end(),
                   symbol.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    rule.symbol = symbol;
    rule.threshold = *threshold;
    rule.source = ".deck/alerts.txt";
    rule.direction = (op == "<=" || op == "<") ? AlertDirection::BelowOrEqual : AlertDirection::AboveOrEqual;
    std::getline(row, rule.note);
    rule.note = trim_copy(rule.note);
    rules.push_back(std::move(rule));
  }
  return rules;
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

std::string market_provider_label(bool has_local_quotes, bool has_finnhub) {
  if (has_local_quotes && has_finnhub) {
    return "local-csv+finnhub";
  }
  if (has_local_quotes) {
    return "local-csv";
  }
  if (has_finnhub) {
    return "finnhub";
  }
  return "none";
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
  std::size_t positions_with_quotes = 0;
  for (const auto& position : runtime.positions) {
    cost_basis_total += position.cost_basis_total;
    if (auto found = runtime.market_quotes.find(position.symbol); found != runtime.market_quotes.end() &&
                                                             found->second.has_data) {
      market_value_total += position.quantity * found->second.last_price;
      daily_change_total += position.quantity * found->second.change;
      ++positions_with_quotes;
    }
  }
  for (const auto& balance : runtime.balances) {
    cash_total += balance.amount;
  }

  {
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(2) << "Cost basis: " << cost_basis_total
            << "  cash: " << cash_total;
    lines.push_back(summary.str());
  }
  {
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(2) << "Market value: " << market_value_total << "  total tracked: "
            << (market_value_total + cash_total);
    lines.push_back(summary.str());
  }
  {
    std::ostringstream summary;
    summary << std::fixed << std::setprecision(2) << "Daily change: " << daily_change_total << "  quoted positions: "
            << positions_with_quotes << "/" << runtime.positions.size();
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
  runtime.files_entries = discover_file_entries(persistent.root);
  runtime.market_entries = discover_market_entries(persistent.root);
  runtime.finance_data_sources = discover_finance_sources(persistent.root);
  runtime.positions = load_positions_from_csv(persistent.root, runtime.finance_data_sources);
  runtime.balances = load_balances_from_csv(persistent.root, runtime.finance_data_sources);
  runtime.alert_rules = load_alert_rules(persistent.root);
  const auto local_quotes = load_local_quotes_from_csv(persistent.root, runtime.finance_data_sources);
  runtime.market_quotes = local_quotes;
  runtime.triggered_alerts = evaluate_alerts(runtime.alert_rules, runtime.market_quotes);
  runtime.market_data_enabled = !local_quotes.empty() || (caps.curl && caps.finnhub_api_key);
  runtime.market_data_provider = market_provider_label(!local_quotes.empty(), caps.curl && caps.finnhub_api_key);
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
                           const EnvironmentCapabilities& caps) {
  const auto token = (caps.curl && caps.finnhub_api_key) ? resolve_env_var(persistent.root, "FINNHUB_API_KEY")
                                                         : std::nullopt;
  const auto local_quotes = load_local_quotes_from_csv(persistent.root, discover_finance_sources(persistent.root));
  if (local_quotes.empty() && !token) {
    set_status(controller, caps.curl ? "No local quote CSV or FINNHUB_API_KEY found" : "No local quote CSV found");
    return;
  }

  std::vector<MarketEntry> watchlist;
  {
    std::lock_guard<std::mutex> lock(controller.mutex);
    if (controller.runtime.market_data_refresh_in_progress) {
      return;
    }
    controller.runtime.market_data_refresh_in_progress = true;
    watchlist = controller.runtime.market_entries;
  }
  screen.PostEvent(ftxui::Event::Custom);

  std::unordered_map<std::string, MarketQuote> quotes;
  bool used_local = false;
  bool used_finnhub = false;
  for (const auto& market : watchlist) {
    if (auto found = local_quotes.find(market.symbol); found != local_quotes.end()) {
      quotes[market.symbol] = found->second;
      used_local = true;
      continue;
    }
    if (token) {
      quotes[market.symbol] = fetch_finnhub_quote(persistent.root, *token, market.symbol);
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
    controller.runtime.triggered_alerts = evaluate_alerts(controller.runtime.alert_rules, controller.runtime.market_quotes);
    controller.runtime.market_data_enabled = used_local || used_finnhub;
    controller.runtime.market_data_provider = market_provider_label(used_local, used_finnhub);
    controller.runtime.market_data_refresh_in_progress = false;
    controller.runtime.portfolio_lines = portfolio_lines_for(controller.runtime);
    controller.runtime.status_message = controller.runtime.triggered_alerts.empty()
                                            ? "Market data refreshed"
                                            : "Alert: " + controller.runtime.triggered_alerts.front().message;
  }
  invalidate_pane_data_snapshot(persistent.root);
  screen.PostEvent(ftxui::Event::Custom);
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
    refresh_market_quotes(screen, controller, persistent, caps);
  }
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
    controller.runtime.diff_hunks.clear();
    controller.runtime.selected_diff_hunk = 0;
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
  std::vector<DiffHunk> diff_hunks;
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
  }

  std::lock_guard<std::mutex> lock(controller.mutex);
  controller.runtime.git_entries = std::move(git_entries);
  if (controller.runtime.git_entries.empty()) {
    controller.runtime.selected_git_index = 0;
  } else if (controller.runtime.selected_git_index >= controller.runtime.git_entries.size()) {
    controller.runtime.selected_git_index = controller.runtime.git_entries.size() - 1;
  }
  controller.runtime.diff_hunks = std::move(diff_hunks);
  if (controller.runtime.diff_hunks.empty()) {
    controller.runtime.selected_diff_hunk = 0;
  } else if (controller.runtime.selected_diff_hunk >= controller.runtime.diff_hunks.size()) {
    controller.runtime.selected_diff_hunk = controller.runtime.diff_hunks.size() - 1;
  }
}

std::vector<std::string> palette_suggestions_for(TabRole role) {
  std::vector<std::string> suggestions = {
      "run",
      "rerun",
      "search <query>",
      "git-status",
      "git-diff",
      "stage <path>",
      "unstage <path>",
      "commit <message>",
      "cancel",
      "quit",
  };
  if (role == TabRole::Finance) {
    suggestions.push_back("refresh");
    suggestions.push_back("add <ticker>");
    suggestions.push_back("focus <ticker>");
  } else {
    suggestions.push_back("refresh");
  }
  return suggestions;
}

void launch_task(ScreenInteractive& screen,
                 ShellTaskController& controller,
                 const WorkspacePersistentState& persistent,
                 std::string name,
                 std::vector<std::string> argv,
                 const EnvironmentCapabilities& caps) {
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
    controller.runtime.task_history.push_back(make_task_record(name, argv));
    task_index = controller.runtime.task_history.size() - 1;
    controller.active_task_index = task_index;
  }
  screen.PostEvent(ftxui::Event::Custom);

  controller.worker = std::jthread([&, task_index, name = std::move(name), argv = std::move(argv)] {
    ProcessRunner runner;
    ProcessRequest request;
    request.argv = argv;
    request.cwd = persistent.root;

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
    }
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
  launch_task(screen, controller, state, name, std::move(argv), caps);
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
                             const WorkspacePersistentState& state,
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
  if (command == "run") {
    if (state.recent_commands.empty()) {
      set_status(controller, "No recent command to run");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    launch_task(screen, controller, state, "recent command", split_command_line(state.recent_commands.front()), caps);
    return true;
  }
  if (command == "rerun") {
    std::vector<std::string> rerun_argv;
    std::string name = "rerun";
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      if (!controller.runtime.task_history.empty()) {
        const auto& task = controller.runtime.task_history.back();
        rerun_argv = task.argv;
        name = task.name + " rerun";
      }
    }
    if (rerun_argv.empty()) {
      set_status(controller, "No task available to rerun");
      screen.PostEvent(ftxui::Event::Custom);
      return true;
    }
    launch_task(screen, controller, state, std::move(name), std::move(rerun_argv), caps);
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
    launch_task(
        screen, controller, state, "git status", {"git", "-C", state.root.string(), "status", "--short", "--branch"}, caps);
    return true;
  }
  if (command == "git-diff") {
    launch_task(screen,
                controller,
                state,
                "git diff",
                {"git", "-C", state.root.string(), "diff", "--stat", "--compact-summary"},
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
      refresh_market_quotes(screen, controller, state, caps);
    } else {
      refresh_git_state(controller, state, caps);
      set_status(controller, "Git state refreshed");
      invalidate_pane_data_snapshot(state.root);
      screen.PostEvent(ftxui::Event::Custom);
    }
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
  Elements commands;
  for (const auto& command : state.recent_commands) {
    commands.push_back(text(command) | color(Color::Yellow));
  }
  if (commands.empty()) {
    commands.push_back(text("No recent commands"));
  }

  Elements doctor_lines;
  for (const auto& line : render_doctor_report(caps)) {
    doctor_lines.push_back(text(line));
  }

  auto overview = vbox({
      text("deck") | bold | color(Color::Cyan),
      text(state.name + "  [" + state.root.string() + "]"),
      text(std::string("safe_mode: ") + (safe_mode ? "on" : "off")),
      text(std::string("surface: ") + (runtime.overlays_enabled ? "interactive text" : "safe text")),
      separator(),
      text("Focused tab") | bold,
      text(current.name + " / " + to_string(current.role)),
      text("Focused pane: " + to_string(current.focused_pane)),
      text("Watchlist entries: " + std::to_string(runtime.market_entries.size())),
      text("Market provider: " + runtime.market_data_provider),
      separator(),
      text("Recent commands") | bold,
      vbox(std::move(commands)),
      separator(),
      text("Environment") | bold,
      vbox(std::move(doctor_lines)),
  });

  auto current_focus = vbox({
      text("Current tab") | bold,
      text(current.name + " / " + to_string(current.role)),
      text("Focused pane: " + to_string(current.focused_pane)),
      separator(),
      text("MVP surface") | bold,
      paragraph(wrap_text(
          "The active tab now owns the full body layout. Runtime task records, Git summaries, search scope, "
          "and finance workspace context are rendered inside the pane tree rather than hidden behind a generic "
          "wrapper.")),
  });

  return vbox({
             window(text(current.name + " Layout"), render_layout_tree(current.layout, snapshot, state, runtime, caps)) |
                 flex,
             hbox({
                 window(text("Workspace"), overview | flex) | flex,
                 window(text("Focus"), current_focus | flex) | flex,
             }),
         }) |
         flex;
}

void launch_ftxui_shell(const WorkspacePersistentState& state,
                        WorkspaceRuntimeState initial_runtime,
                        const EnvironmentCapabilities& caps,
                        bool safe_mode) {
  ShellTaskController controller;
  controller.runtime = std::move(initial_runtime);
  controller.runtime.visible_tab = state.focused_tab;
  SearchOverlayState search_overlay;
  TickerOverlayState ticker_overlay;
  CommandOverlayState command_overlay;
  CommitOverlayState commit_overlay;

  int tab_index = static_cast<int>(controller.runtime.visible_tab);
  std::vector<std::string> tab_names;
  tab_names.reserve(state.tabs.size());
  for (const auto& tab : state.tabs) {
    tab_names.push_back(tab.name);
  }

  auto tabs = Toggle(&tab_names, &tab_index);
  auto footer = Renderer([&] {
    WorkspaceRuntimeState runtime_snapshot;
    {
      std::lock_guard<std::mutex> lock(controller.mutex);
      runtime_snapshot = controller.runtime;
    }
    const auto& current = state.tabs[runtime_snapshot.visible_tab];
    return hbox({
               text("role: " + to_string(current.role)),
               separator(),
               text("pane: " + to_string(current.focused_pane)),
               separator(),
               text(std::string("scratch: ") + (runtime_snapshot.scratch_editor.dirty ? "dirty" : "saved")),
               separator(),
               text("watchlist: " + std::to_string(runtime_snapshot.market_entries.size())),
               separator(),
               text("market: " + runtime_snapshot.market_data_provider),
               separator(),
               text("status: " + (runtime_snapshot.status_message.empty() ? std::string("ready")
                                                                          : runtime_snapshot.status_message)),
               separator(),
               text("r run"),
               separator(),
               text("R rerun"),
               separator(),
               text("a add"),
               separator(),
               text("s/u git"),
               separator(),
               text("c commit"),
               separator(),
               text("x refresh/cancel"),
               separator(),
               text(": palette"),
               separator(),
               text("q quit"),
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
      }
      runtime_snapshot = controller.runtime;
    }
    if (!state.tabs.empty()) {
      runtime_snapshot.visible_tab =
          static_cast<std::size_t>(std::clamp(tab_index, 0, static_cast<int>(state.tabs.size() - 1)));
    }
    auto tab_bar = tabs->Render() | border | color(Color::Green);
    auto body = render_summary(state, runtime_snapshot, caps, safe_mode);
    Element content = vbox({
               tab_bar,
               body,
               footer->Render() | border | dim,
           }) |
           borderHeavy | size(WIDTH, GREATER_THAN, 100) | size(HEIGHT, GREATER_THAN, 28);
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
    return content;
  });

  auto screen = ScreenInteractive::Fullscreen();
  if (controller.runtime.market_data_enabled) {
    controller.market_worker = std::jthread([&](std::stop_token stop_token) {
      while (!stop_token.stop_requested()) {
        refresh_market_quotes(screen, controller, state, caps);
        for (int i = 0; i < 150 && !stop_token.stop_requested(); ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    });
  }
  auto root = CatchEvent(renderer, [&](ftxui::Event event) {
    if (event == ftxui::Event::Character('r')) {
      if (!state.recent_commands.empty()) {
        auto argv = split_command_line(state.recent_commands.front());
        launch_task(screen, controller, state, "recent command", std::move(argv), caps);
      }
      return true;
    }
    if (event == ftxui::Event::Character('R')) {
      std::vector<std::string> argv;
      std::string name = "rerun";
      {
        std::lock_guard<std::mutex> lock(controller.mutex);
        if (!controller.runtime.task_history.empty()) {
          const auto& task = controller.runtime.task_history.back();
          argv = task.argv;
          name = task.name + " rerun";
        }
      }
      if (!argv.empty()) {
        launch_task(screen, controller, state, std::move(name), std::move(argv), caps);
      }
      return true;
    }
    if (event == ftxui::Event::Character('x')) {
      if (controller.task_running.load()) {
        controller.cancel_requested = true;
      } else {
        const auto role = [&] {
          std::lock_guard<std::mutex> lock(controller.mutex);
          return state.tabs[controller.runtime.visible_tab].role;
        }();
        if (role == TabRole::Finance) {
          refresh_market_quotes(screen, controller, state, caps);
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
        } else if (role == TabRole::Review) {
          if (!controller.runtime.git_entries.empty()) {
            const auto before = controller.runtime.selected_git_index;
            controller.runtime.selected_git_index =
                std::min(controller.runtime.selected_git_index + 1, controller.runtime.git_entries.size() - 1);
            review_changed = controller.runtime.selected_git_index != before;
          }
        } else if (!controller.runtime.files_entries.empty()) {
          controller.runtime.selected_file_index =
              std::min(controller.runtime.selected_file_index + 1, controller.runtime.files_entries.size() - 1);
          refresh_note_context(state.root, controller.runtime);
        }
      }
      if (review_changed) {
        refresh_git_state(controller, state, caps);
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
        } else if (role == TabRole::Review) {
          if (controller.runtime.selected_git_index > 0) {
            --controller.runtime.selected_git_index;
            review_changed = true;
          }
        } else if (controller.runtime.selected_file_index > 0) {
          --controller.runtime.selected_file_index;
          refresh_note_context(state.root, controller.runtime);
        }
      }
      if (review_changed) {
        refresh_git_state(controller, state, caps);
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
          return true;
        }
      }
      if (role == TabRole::Review) {
        std::optional<GitStatusEntry> selected_git;
        int target_line = 1;
        {
          std::lock_guard<std::mutex> lock(controller.mutex);
          if (!controller.runtime.git_entries.empty() &&
              controller.runtime.selected_git_index < controller.runtime.git_entries.size()) {
            selected_git = controller.runtime.git_entries[controller.runtime.selected_git_index];
          }
          if (!controller.runtime.diff_hunks.empty() &&
              controller.runtime.selected_diff_hunk < controller.runtime.diff_hunks.size()) {
            target_line = std::max(controller.runtime.diff_hunks[controller.runtime.selected_diff_hunk].new_start, 1);
          }
        }
        if (selected_git) {
          SearchResult file_result;
          file_result.path = selected_git->path;
          file_result.line = target_line;
          open_in_nvim(screen, state, file_result);
          set_status(controller, "Opened " + selected_git->path);
          invalidate_pane_data_snapshot(state.root);
          screen.PostEvent(ftxui::Event::Custom);
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
        open_in_nvim(screen, state, *selected);
        set_status(controller, "Opened search result");
        invalidate_pane_data_snapshot(state.root);
        screen.PostEvent(ftxui::Event::Custom);
        return true;
      }
      std::optional<FileEntry> file_entry;
      {
        std::lock_guard<std::mutex> lock(controller.mutex);
        if (!controller.runtime.files_entries.empty() &&
            controller.runtime.selected_file_index < controller.runtime.files_entries.size()) {
          file_entry = controller.runtime.files_entries[controller.runtime.selected_file_index];
          refresh_note_context(state.root, controller.runtime);
        }
      }
      if (file_entry) {
        SearchResult file_result;
        file_result.path = file_entry->path;
        file_result.line = 1;
        open_in_nvim(screen, state, file_result);
        set_status(controller, "Opened " + file_entry->path);
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
}

std::filesystem::path normalize_workspace_path(const CliOptions& options) {
  if (options.workspace_open || options.workspace_reset_layout) {
    return std::filesystem::absolute(options.workspace_path);
  }
  return std::filesystem::current_path();
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
  }

  WorkspaceRuntimeState runtime;
  runtime.overlays_enabled = !options.safe_mode;
  bootstrap_runtime_state(persistent, caps, options.safe_mode, runtime);

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
