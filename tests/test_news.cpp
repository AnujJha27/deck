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
  const std::string json = R"({"hits":[{"_highlightResult":{"title":{"value":"nested metadata"}},"title":"AI \"progress\"","url":"https://example.com/a","author":"ada","created_at":"2026-07-17","story_text":"A <b>useful</b> text post &amp; discussion"},{"title":"Web3 incident","url":null,"objectID":"42","author":"lin"}]})";
  const auto entries = deck::parse_hacker_news_response(json, "AI");
  DECK_ASSERT(entries.size() == 2);
  DECK_ASSERT(entries[0].title == "AI \"progress\"");
  DECK_ASSERT(entries[1].url == "https://news.ycombinator.com/item?id=42");
  DECK_ASSERT(entries[0].summary == "A useful text post & discussion");
}

DECK_TEST(article_preview_preserves_readable_sections_and_discards_boilerplate) {
  const auto text = deck::readable_article_text(
      "<html><head><title>duplicate</title></head><header>menu</header><body><h1>Title</h1>"
      "<p>Hello   world</p><h2>Details</h2><p>Useful article text.</p><footer>subscribe</footer></body></html>");
  DECK_ASSERT(text == "# Title\n\nHello world\n\n## Details\n\nUseful article text.");
}

DECK_TEST(article_preview_rejects_local_or_non_https_urls) {
  DECK_ASSERT(deck::safe_article_url("https://example.com/story"));
  DECK_ASSERT(!deck::safe_article_url("http://example.com/story"));
  DECK_ASSERT(!deck::safe_article_url("https://localhost/story"));
  DECK_ASSERT(!deck::safe_article_url("https://192.168.1.2/story"));
  DECK_ASSERT(!deck::safe_article_url("https://user@127.0.0.1/story"));
}
