#pragma once

#include "deck/environment.h"
#include "deck/workspace.h"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace deck {

std::vector<MarketEntry> discover_market_entries(const std::filesystem::path& root);
bool persist_watchlist(const std::filesystem::path& root, const std::vector<MarketEntry>& entries);
std::vector<std::string> discover_finance_sources(const std::filesystem::path& root);
std::string lower_copy(std::string value);
std::string trim_copy(std::string value);
std::vector<PositionEntry> load_positions_from_csv(const std::filesystem::path& root,
                                                   const std::vector<std::string>& sources);
std::vector<BalanceEntry> load_balances_from_csv(const std::filesystem::path& root,
                                                 const std::vector<std::string>& sources);
std::unordered_map<std::string, MarketQuote> load_local_quotes_from_csv(
    const std::filesystem::path& root, const std::vector<std::string>& sources);
std::unordered_map<std::string, std::vector<MarketCandle>> load_local_candles_from_csv(
    const std::filesystem::path& root, const std::vector<std::string>& sources);
std::vector<MarketCandle> fetch_twelve_data_candles(const std::filesystem::path& root,
                                                    const std::string& token,
                                                    const std::string& symbol);
MarketQuote quote_from_candles(const std::string& symbol,
                               const std::vector<MarketCandle>& candles,
                               const std::string& provider);
std::optional<AlertRule> parse_alert_rule_line(std::string line, const std::string& source);
std::vector<AlertRule> load_alert_rules(const std::filesystem::path& root);
bool persist_alert_rules(const std::filesystem::path& root, const std::vector<AlertRule>& rules);
std::vector<TriggeredAlert> evaluate_alerts(
    const std::vector<AlertRule>& rules,
    const std::unordered_map<std::string, MarketQuote>& quotes);
std::string market_provider_label(bool has_local, bool has_twelve_data, bool has_finnhub);
std::vector<std::string> portfolio_lines_for(const WorkspaceRuntimeState& runtime);
MarketQuote fetch_finnhub_quote(const std::filesystem::path& root,
                                const std::string& token,
                                const std::string& symbol);

}  // namespace deck
