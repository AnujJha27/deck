#include "deck/panes.h"

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
