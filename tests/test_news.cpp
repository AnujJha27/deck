#include "deck/news.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace deck::test { using TestFn = std::function<void()>; std::vector<std::pair<std::string, TestFn>>& registry(); }
#define DECK_TEST(name) void name(); namespace { const bool name##_registered = [] { deck::test::registry().push_back({#name, name}); return true; }(); } void name()
#define DECK_ASSERT(condition) do { if (!(condition)) throw std::runtime_error("assertion failed: " #condition); } while (false)

DECK_TEST(news_parser_reads_hits_and_falls_back_to_discussion_url) {
  const std::string json = R"({"hits":[{"_highlightResult":{"title":{"value":"nested metadata"}},"title":"AI \"progress\"","url":"https://example.com/a","author":"ada","created_at":"2026-07-17"},{"title":"Web3 incident","url":null,"objectID":"42","author":"lin"}]})";
  const auto entries = deck::parse_hacker_news_response(json, "AI");
  DECK_ASSERT(entries.size() == 2);
  DECK_ASSERT(entries[0].title == "AI \"progress\"");
  DECK_ASSERT(entries[1].url == "https://news.ycombinator.com/item?id=42");
}
