#include <exception>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace deck::test {
using TestFn = std::function<void()>;
std::vector<std::pair<std::string, TestFn>>& registry() {
  static std::vector<std::pair<std::string, TestFn>> value;
  return value;
}
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

int main() {
  int failures = 0;
  for (const auto& [name, fn] : deck::test::registry()) {
    try {
      fn();
    } catch (const std::exception& ex) {
      ++failures;
      std::cerr << name << ": " << ex.what() << "\n";
    }
  }
  return failures == 0 ? 0 : 1;
}
