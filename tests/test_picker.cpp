#include "deck/picker.h"

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

DECK_TEST(picker_fuzzy_score_prefers_exact_and_word_prefixes) {
  DECK_ASSERT(deck::fuzzy_score("build", "build") > deck::fuzzy_score("build", "CMake: build"));
  DECK_ASSERT(deck::fuzzy_score("cb", "CMake: build") > 0);
  DECK_ASSERT(deck::fuzzy_score("xyz", "CMake: build") < 0);
}

DECK_TEST(picker_discovers_cmake_recipes_without_a_shell) {
  auto root = std::filesystem::current_path();
  if (!std::filesystem::exists(root / "CMakeLists.txt")) {
    root = root.parent_path();
  }
  const auto recipes = deck::discover_project_recipes(root);
  DECK_ASSERT(!recipes.empty());
  DECK_ASSERT(recipes.front().argv.front() == "cmake");
}
