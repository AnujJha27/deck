#include "deck/persistence.h"

#include <filesystem>
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
      throw std::runtime_error("assertion failed: " #condition);               \
    }                                                                           \
  } while (false)

DECK_TEST(task_history_redacts_and_omits_sensitive_argv) {
  const auto root = std::filesystem::temp_directory_path() / "deck-task-history-test";
  std::filesystem::remove_all(root);
  deck::TaskRecord task;
  task.name = "request";
  task.command = "curl --token top-secret";
  task.argv = {"curl", "--token", "top-secret"};
  task.state = deck::TaskState::Exited;
  task.stdout_excerpt = "Authorization: Bearer top-secret\napi_key=also-secret";
  deck::WorkspaceStore store;
  DECK_ASSERT(store.save_tasks(root, {task}));
  const auto loaded = store.load_tasks(root);
  DECK_ASSERT(loaded.size() == 1);
  DECK_ASSERT(loaded[0].argv.empty());
  DECK_ASSERT(loaded[0].command == "[sensitive command omitted]");
  DECK_ASSERT(loaded[0].stdout_excerpt.find("top-secret") == std::string::npos);
  DECK_ASSERT(loaded[0].stdout_excerpt.find("also-secret") == std::string::npos);
  DECK_ASSERT(store.clear_tasks(root));
  DECK_ASSERT(store.load_tasks(root).empty());
  std::filesystem::remove_all(root);
}

DECK_TEST(math_history_keeps_latest_hundred_entries) {
  const auto root = std::filesystem::temp_directory_path() / "deck-math-history-test";
  std::filesystem::remove_all(root);
  std::vector<std::pair<std::string, std::string>> history;
  for (int i = 0; i < 105; ++i) {
    history.push_back({"x+" + std::to_string(i), std::to_string(i)});
  }
  deck::WorkspaceStore store;
  DECK_ASSERT(store.save_math_history(root, history));
  const auto loaded = store.load_math_history(root);
  DECK_ASSERT(loaded.size() == 100);
  DECK_ASSERT(loaded.front().first == "x+5");
  DECK_ASSERT(loaded.back().first == "x+104");
  std::filesystem::remove_all(root);
}
