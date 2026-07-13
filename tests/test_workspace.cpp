#include "deck/workspace.h"
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

DECK_TEST(workspace_default_tabs) {
  auto state = deck::make_default_workspace("/tmp/deck");
  DECK_ASSERT(state.tabs.size() == 5);
  DECK_ASSERT(state.tabs[0].name == "Dev");
  DECK_ASSERT(deck::contains_pane(state.tabs[1].layout, deck::PaneKind::Tasks));
  DECK_ASSERT(state.tabs[3].role == deck::TabRole::Finance);
  DECK_ASSERT(state.tabs[4].role == deck::TabRole::Notes);
}

DECK_TEST(workspace_round_trip) {
  auto state = deck::make_default_workspace("/tmp/deck");
  state.last_anchor = deck::PaperAnchor{8, 0.45, "transformers", "Attention"};
  auto parsed = deck::parse_workspace(deck::serialize_workspace(state), state.root);
  DECK_ASSERT(parsed.has_value());
  DECK_ASSERT(parsed->tabs.size() == state.tabs.size());
  DECK_ASSERT(parsed->last_anchor.has_value());
  DECK_ASSERT(parsed->last_anchor->page == 8);
}

DECK_TEST(workspace_tab_upgrade_adds_missing_notes_tab) {
  auto state = deck::make_default_workspace("/tmp/deck");
  state.tabs.pop_back();
  deck::ensure_workspace_tabs(state);
  DECK_ASSERT(state.tabs.size() == 5);
  DECK_ASSERT(state.tabs.back().role == deck::TabRole::Notes);
  DECK_ASSERT(deck::contains_pane(state.tabs.back().layout, deck::PaneKind::Scratch));
}
