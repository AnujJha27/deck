#include "deck/finance.h"

#include "deck/process.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <initializer_list>
#include <sstream>
#include <string_view>
#include <system_error>

namespace deck {
namespace {

std::string clip_text(std::string text, std::size_t limit = 240) {
  if (text.size() <= limit) return text;
  return text.substr(0, limit) + "...";
}

std::vector<FileEntry> discover_file_entries(const std::filesystem::path& root) {
  std::vector<FileEntry> entries;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(
           root, std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (ec) break;
    const auto name = entry.path().filename().string();
    if (name == ".git" || name == ".deck" || name == "build") continue;
    if (entry.is_regular_file(ec)) entries.push_back({name, false});
    ec.clear();
  }
  std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
    return left.path < right.path;
  });
  if (entries.size() > 64) entries.resize(64);
  return entries;
}

}  // namespace


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

}  // namespace deck
