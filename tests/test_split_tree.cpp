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

DECK_TEST(split_tree_leaf_order_and_nearest_resize) {
  auto tree = deck::make_split(
      deck::SplitAxis::Horizontal,
      0.4,
      deck::make_leaf(deck::PaneKind::Files),
      deck::make_split(deck::SplitAxis::Vertical,
                       0.7,
                       deck::make_leaf(deck::PaneKind::Terminal),
                       deck::make_leaf(deck::PaneKind::Git)));
  const auto order = deck::pane_order(tree);
  DECK_ASSERT(order.size() == 3);
  DECK_ASSERT(order[0] == deck::PaneKind::Files);
  DECK_ASSERT(order[1] == deck::PaneKind::Terminal);
  DECK_ASSERT(order[2] == deck::PaneKind::Git);

  DECK_ASSERT(deck::resize_nearest_split(
      tree, deck::PaneKind::Terminal, deck::SplitAxis::Vertical, 0.25));
  DECK_ASSERT(deck::serialize_split_tree(tree).find("0.9") != std::string::npos);
  DECK_ASSERT(deck::resize_nearest_split(
      tree, deck::PaneKind::Terminal, deck::SplitAxis::Horizontal, -0.5));
  DECK_ASSERT(deck::serialize_split_tree(tree).find("0.1") != std::string::npos);
}
