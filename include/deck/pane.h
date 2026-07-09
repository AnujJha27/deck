#pragma once

#include "deck/types.h"

#include <ftxui/component/component.hpp>

namespace deck {

using PaneId = PaneKind;

class Pane {
 public:
  virtual ~Pane() = default;
  virtual PaneId id() const = 0;
  virtual ftxui::Component component() = 0;

  virtual void on_focus() {}
  virtual void on_blur() {}
  virtual void on_workspace_open() {}
  virtual void on_workspace_close() {}
  virtual void on_tick() {}

  virtual PaneStatus status() const = 0;
};

}  // namespace deck
