#pragma once

#include "deck/types.h"

#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace deck {

struct OpenFileEvent {
  std::string path;
};

struct OpenPaperEvent {
  std::string path;
};

struct OpenNoteEvent {
  std::string path;
};

struct RunTaskEvent {
  std::string name;
  std::vector<std::string> argv;
};

struct TaskStartedEvent {
  std::string name;
};

struct TaskFinishedEvent {
  std::string name;
  int exit_code = 0;
};

struct GitRefreshRequestedEvent {};
struct SearchCompletedEvent {
  std::string query;
  std::size_t result_count = 0;
};
struct WorkspaceChangedEvent {
  std::string name;
};
struct TabChangedEvent {
  std::string name;
};
struct OverlayInvalidatedEvent {};

using Event = std::variant<OpenFileEvent,
                           OpenPaperEvent,
                           OpenNoteEvent,
                           RunTaskEvent,
                           TaskStartedEvent,
                           TaskFinishedEvent,
                           GitRefreshRequestedEvent,
                           SearchCompletedEvent,
                           WorkspaceChangedEvent,
                           TabChangedEvent,
                           OverlayInvalidatedEvent>;

class EventBus {
 public:
  using Listener = std::function<void(const Event&)>;

  void subscribe(Listener listener);
  void publish(const Event& event) const;

 private:
  std::vector<Listener> listeners_;
};

}  // namespace deck
