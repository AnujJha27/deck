#include "deck/event_bus.h"

namespace deck {

void EventBus::subscribe(Listener listener) { listeners_.push_back(std::move(listener)); }

void EventBus::publish(const Event& event) const {
  for (const auto& listener : listeners_) {
    listener(event);
  }
}

}  // namespace deck
