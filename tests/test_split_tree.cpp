#include "deck/split_tree.h"

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

DECK_TEST(split_tree_round_trip) {
  auto tree = deck::make_split(
      deck::SplitAxis::Horizontal,
      0.25,
      deck::make_leaf(deck::PaneKind::Files),
      deck::make_split(deck::SplitAxis::Vertical,
                       0.7,
                       deck::make_leaf(deck::PaneKind::Terminal),
                       deck::make_leaf(deck::PaneKind::Git)));
  const auto text = deck::serialize_split_tree(tree);
  auto parsed = deck::parse_split_tree(text);
  DECK_ASSERT(parsed.has_value());
  DECK_ASSERT(deck::serialize_split_tree(*parsed) == text);
}

DECK_TEST(split_tree_resize) {
  auto tree = deck::make_split(deck::SplitAxis::Horizontal,
                               0.2,
                               deck::make_leaf(deck::PaneKind::Files),
                               deck::make_leaf(deck::PaneKind::Git));
  DECK_ASSERT(deck::resize_first_match(tree, deck::PaneKind::Files, 0.6));
  DECK_ASSERT(deck::serialize_split_tree(tree).find("0.6") != std::string::npos);
}
