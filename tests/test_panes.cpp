#include "deck/panes.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
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

DECK_TEST(parse_git_status_entries_handles_common_statuses) {
  const std::string input =
      "## main\n"
      "M  src/app.cpp\n"
      " M README.md\n"
      "R  old.txt -> new.txt\n"
      "?? scratch.txt\n";

  auto entries = deck::parse_git_status_entries(input);
  DECK_ASSERT(entries.size() == 4);
  DECK_ASSERT(entries[0].path == "src/app.cpp");
  DECK_ASSERT(entries[0].index_status == "M");
  DECK_ASSERT(entries[0].worktree_status == " ");
  DECK_ASSERT(entries[1].path == "README.md");
  DECK_ASSERT(entries[1].index_status == " ");
  DECK_ASSERT(entries[1].worktree_status == "M");
  DECK_ASSERT(entries[2].renamed);
  DECK_ASSERT(entries[2].original_path == "old.txt");
  DECK_ASSERT(entries[2].path == "new.txt");
  DECK_ASSERT(entries[3].index_status == "?");
  DECK_ASSERT(entries[3].worktree_status == "?");
}

DECK_TEST(parse_git_branch_entries_marks_current_branch) {
  const std::string input =
      "  feature/search\n"
      "* main\n"
      "  release/1.0\n";

  auto entries = deck::parse_git_branch_entries(input);
  DECK_ASSERT(entries.size() == 3);
  DECK_ASSERT(entries[0].name == "feature/search");
  DECK_ASSERT(!entries[0].current);
  DECK_ASSERT(entries[1].name == "main");
  DECK_ASSERT(entries[1].current);
}

DECK_TEST(parse_diff_hunks_handles_unified_headers) {
  const std::string input =
      "diff --git a/src/app.cpp b/src/app.cpp\n"
      "@@ -10,2 +10,3 @@ context\n"
      "-old\n"
      "+new\n"
      "@@ -40 +41,2 @@ more\n";

  auto hunks = deck::parse_diff_hunks(input);
  DECK_ASSERT(hunks.size() == 2);
  DECK_ASSERT(hunks[0].old_start == 10);
  DECK_ASSERT(hunks[0].old_count == 2);
  DECK_ASSERT(hunks[0].new_start == 10);
  DECK_ASSERT(hunks[0].new_count == 3);
  DECK_ASSERT(hunks[1].old_start == 40);
  DECK_ASSERT(hunks[1].old_count == 1);
  DECK_ASSERT(hunks[1].new_start == 41);
  DECK_ASSERT(hunks[1].new_count == 2);
}

DECK_TEST(build_patch_for_hunk_extracts_selected_hunk) {
  const std::string input =
      "diff --git a/src/app.cpp b/src/app.cpp\n"
      "index 1111111..2222222 100644\n"
      "--- a/src/app.cpp\n"
      "+++ b/src/app.cpp\n"
      "@@ -10,2 +10,3 @@ context\n"
      "-old\n"
      "+new\n"
      " keep\n"
      "@@ -40 +41,2 @@ more\n"
      "-gone\n"
      "+back\n";

  auto patch = deck::build_patch_for_hunk(input, 1);
  DECK_ASSERT(patch.has_value());
  DECK_ASSERT(patch->find("diff --git a/src/app.cpp b/src/app.cpp") != std::string::npos);
  DECK_ASSERT(patch->find("@@ -40 +41,2 @@ more") != std::string::npos);
  DECK_ASSERT(patch->find("@@ -10,2 +10,3 @@ context") == std::string::npos);
}

DECK_TEST(file_browser_includes_selected_text_preview) {
  const auto root = std::filesystem::temp_directory_path() / "deck_file_preview_test";
  std::filesystem::create_directories(root);
  {
    std::ofstream output(root / "example.txt");
    output << "first preview line\nsecond preview line\n";
  }

  auto state = deck::make_default_workspace(root);
  deck::WorkspaceRuntimeState runtime;
  runtime.files_entries = {{"example.txt", false}};
  auto snapshot = deck::build_pane_data_snapshot(state, runtime, {});

  DECK_ASSERT(std::find(snapshot.files_lines.begin(), snapshot.files_lines.end(), "Preview: example.txt") !=
              snapshot.files_lines.end());
  DECK_ASSERT(std::find(snapshot.files_lines.begin(), snapshot.files_lines.end(), "  first preview line") !=
              snapshot.files_lines.end());

  deck::invalidate_pane_data_snapshot(root);
  std::filesystem::remove_all(root);
}

DECK_TEST(git_rows_show_inline_stage_and_unstage_actions) {
  auto state = deck::make_default_workspace("/tmp/deck");
  deck::WorkspaceRuntimeState runtime;
  runtime.git_status_text = " M README.md\nM  src/app.cpp\n";
  runtime.git_entries = {{"README.md", " ", "M"}, {"src/app.cpp", "M", " "}};
  deck::EnvironmentCapabilities caps;
  caps.git = true;
  auto snapshot = deck::build_pane_data_snapshot(state, runtime, caps);

  DECK_ASSERT(std::any_of(snapshot.git_lines.begin(), snapshot.git_lines.end(), [](const std::string& line) {
    return line.find("README.md") != std::string::npos && line.find("[s stage]") != std::string::npos;
  }));
  DECK_ASSERT(std::any_of(snapshot.git_lines.begin(), snapshot.git_lines.end(), [](const std::string& line) {
    return line.find("src/app.cpp") != std::string::npos && line.find("[u unstage]") != std::string::npos;
  }));
}

DECK_TEST(finance_chart_renders_ohlc_candles) {
  const auto root = std::filesystem::temp_directory_path() / "deck_candle_chart_test";
  auto state = deck::make_default_workspace(root);
  deck::WorkspaceRuntimeState runtime;
  runtime.current_market_symbol = "TEST";
  runtime.market_candles["TEST"] = {
      {"2026-07-15", 10.0, 12.0, 9.0, 11.5, 1000.0},
      {"2026-07-16", 11.5, 13.0, 10.5, 11.0, 1200.0},
  };
  const auto snapshot = deck::build_pane_data_snapshot(state, runtime, {});

  DECK_ASSERT(std::any_of(snapshot.portfolio_lines.begin(), snapshot.portfolio_lines.end(), [](const std::string& line) {
    return line.find("TEST  ·  1D") != std::string::npos;
  }));
  DECK_ASSERT(std::any_of(snapshot.portfolio_lines.begin(), snapshot.portfolio_lines.end(), [](const std::string& line) {
    return line.find("2026-07-15") != std::string::npos && line.find("2026-07-16") != std::string::npos;
  }));
  DECK_ASSERT(snapshot.portfolio_lines.size() > 2);
  DECK_ASSERT(snapshot.portfolio_lines[1].find("│ ") == std::string::npos);
  DECK_ASSERT(snapshot.portfolio_lines[1].find("█ ") == std::string::npos);
  DECK_ASSERT(snapshot.portfolio_lines[1].find("▓ ") == std::string::npos);

  deck::invalidate_pane_data_snapshot(root);
}

DECK_TEST(news_preview_scrolls_through_long_article_text) {
  const auto root = std::filesystem::temp_directory_path() / "deck_news_scroll_test";
  auto state = deck::make_default_workspace(root);
  deck::WorkspaceRuntimeState runtime;
  std::string article = "FIRST_MARKER ";
  for (int i = 0; i < 120; ++i) article += "articleword ";
  article += "LATER_MARKER";
  runtime.news_entries = {{"AI World", "Long article", "https://example.com/story", "Example", "now", article}};
  runtime.news_preview_scroll = 10;

  const auto snapshot = deck::build_pane_data_snapshot(state, runtime, {});
  const auto contains = [&](const std::string& needle) {
    return std::any_of(snapshot.news_preview_lines.begin(), snapshot.news_preview_lines.end(),
                       [&](const std::string& line) { return line.find(needle) != std::string::npos; });
  };
  DECK_ASSERT(!contains("FIRST_MARKER"));
  DECK_ASSERT(contains("LATER_MARKER"));
  DECK_ASSERT(contains("PgUp/PgDn scroll"));

  deck::invalidate_pane_data_snapshot(root);
}

DECK_TEST(note_snapshots_keep_lines_available_for_pane_scrolling) {
  const auto root = std::filesystem::temp_directory_path() / "deck_note_scroll_test";
  auto state = deck::make_default_workspace(root);
  deck::WorkspaceRuntimeState runtime;
  for (int i = 0; i < 40; ++i) runtime.scratch_editor.buffer += "scratch line " + std::to_string(i) + "\n";

  const auto snapshot = deck::build_pane_data_snapshot(state, runtime, {});
  DECK_ASSERT(snapshot.scratch_lines.size() > 40);
  DECK_ASSERT(std::any_of(snapshot.scratch_lines.begin(), snapshot.scratch_lines.end(), [](const std::string& line) {
    return line.find("scratch line 39") != std::string::npos;
  }));

  deck::invalidate_pane_data_snapshot(root);
}
