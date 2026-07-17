#include "deck/workspace.h"
#include "deck/split_tree.h"

#include <algorithm>
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
  DECK_ASSERT(state.tabs.size() == 6);
  DECK_ASSERT(state.tabs[0].name == "Dev");
  DECK_ASSERT(deck::contains_pane(state.tabs[1].layout, deck::PaneKind::Tasks));
  DECK_ASSERT(state.tabs[3].role == deck::TabRole::Finance);
  DECK_ASSERT(state.tabs[4].role == deck::TabRole::Notes);
  DECK_ASSERT(state.tabs[5].role == deck::TabRole::Math);
  DECK_ASSERT(deck::contains_pane(state.tabs[5].layout, deck::PaneKind::MathPlot));
}

DECK_TEST(workspace_round_trip) {
  auto state = deck::make_default_workspace("/tmp/deck");
  state.last_anchor = deck::PaperAnchor{8, 0.45, "transformers", "Attention"};
  state.external_editor = "vscode";
  auto parsed = deck::parse_workspace(deck::serialize_workspace(state), state.root);
  DECK_ASSERT(parsed.has_value());
  DECK_ASSERT(parsed->tabs.size() == state.tabs.size());
  DECK_ASSERT(parsed->last_anchor.has_value());
  DECK_ASSERT(parsed->last_anchor->page == 8);
  DECK_ASSERT(parsed->external_editor == "vscode");
}

DECK_TEST(workspace_invalid_editor_falls_back_to_neovim) {
  auto state = deck::make_default_workspace("/tmp/deck");
  state.external_editor = "emacs";
  auto parsed = deck::parse_workspace(deck::serialize_workspace(state), state.root);
  DECK_ASSERT(parsed.has_value());
  DECK_ASSERT(parsed->external_editor == "nvim");
}

DECK_TEST(workspace_tab_upgrade_adds_missing_notes_tab) {
  auto state = deck::make_default_workspace("/tmp/deck");
  state.tabs.erase(state.tabs.begin() + 4);
  deck::ensure_workspace_tabs(state);
  DECK_ASSERT(state.tabs.size() == 6);
  const auto notes = std::find_if(state.tabs.begin(), state.tabs.end(), [](const auto& tab) {
    return tab.role == deck::TabRole::Notes;
  });
  DECK_ASSERT(notes != state.tabs.end());
  DECK_ASSERT(deck::contains_pane(notes->layout, deck::PaneKind::Scratch));
}

DECK_TEST(workspace_tab_upgrade_adds_missing_math_tab) {
  auto state = deck::make_default_workspace("/tmp/deck");
  state.tabs.pop_back();
  deck::ensure_workspace_tabs(state);
  DECK_ASSERT(state.tabs.size() == 6);
  DECK_ASSERT(state.tabs.back().role == deck::TabRole::Math);
  DECK_ASSERT(deck::contains_pane(state.tabs.back().layout, deck::PaneKind::MathResult));
}
