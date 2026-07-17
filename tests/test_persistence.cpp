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
