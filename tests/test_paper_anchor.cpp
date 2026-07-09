#include "deck/types.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace deck::test {
using TestFn = std::function<void()>;
std::vector<std::pair<std::string, TestFn>>& registry();
}  // namespace deck::test

#define DECK_TEST(name)                                                         \
  void name();                                                                  \
  namespace {                                                                   \
  const bool name##_registered = [] {                                           \
    deck::test::registry().push_back({#name, name});                            \
    return true;                                                                \
  }();                                                                          \
  }                                                                             \
  void name()

#define DECK_ASSERT(condition)                                                  \
  do {                                                                          \
    if (!(condition)) {                                                         \
      throw std::runtime_error("assertion failed: " #condition);                \
    }                                                                           \
  } while (false)

DECK_TEST(paper_anchor_round_trip) {
  deck::PaperAnchor anchor{4, 0.25, "quote", "intro"};
  auto parsed = deck::parse_anchor(deck::serialize_anchor(anchor));
  DECK_ASSERT(parsed.has_value());
  DECK_ASSERT(parsed->page == 4);
  DECK_ASSERT(parsed->normalized_y.has_value());
  DECK_ASSERT(parsed->text_quote == std::optional<std::string>{"quote"});
}
