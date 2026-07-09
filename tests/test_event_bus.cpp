#include "deck/event_bus.h"

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

DECK_TEST(event_bus_routes_events) {
  deck::EventBus bus;
  bool seen = false;
  bus.subscribe([&](const deck::Event& event) {
    if (std::holds_alternative<deck::TaskStartedEvent>(event)) {
      seen = true;
    }
  });
  bus.publish(deck::TaskStartedEvent{"build"});
  DECK_ASSERT(seen);
}
