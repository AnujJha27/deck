#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace deck {

enum class PaneKind {
  Files,
  Terminal,
  Search,
  Git,
  Logs,
  Tasks,
  Markets,
  Portfolio,
  Notes,
  Scratch,
  Diff,
};

enum class TabRole {
  Dev,
  Review,
  Run,
  Finance,
  Notes,
};

enum class SplitAxis {
  Horizontal,
  Vertical,
};

enum class PaneStatus {
  Idle,
  Busy,
  Ready,
  Degraded,
};

enum class TaskState {
  Idle,
  Starting,
  Running,
  Exited,
  Failed,
  Cancelled,
};

struct PaperAnchor {
  int page = 1;
  std::optional<double> normalized_y;
  std::optional<std::string> text_quote;
  std::optional<std::string> section_hint;
};

struct PaneLeaf {
  PaneKind kind = PaneKind::Files;
};

struct SplitNode;

struct SplitBranch {
  SplitAxis axis = SplitAxis::Horizontal;
  double ratio = 0.5;
  std::unique_ptr<SplitNode> first;
  std::unique_ptr<SplitNode> second;

  SplitBranch();
  SplitBranch(SplitAxis axis,
              double ratio,
              std::unique_ptr<SplitNode> first,
              std::unique_ptr<SplitNode> second);
  SplitBranch(const SplitBranch& other);
  SplitBranch& operator=(const SplitBranch& other);
  SplitBranch(SplitBranch&& other) noexcept;
  SplitBranch& operator=(SplitBranch&& other) noexcept;
  ~SplitBranch();
};

struct SplitNode {
  std::variant<PaneLeaf, SplitBranch> node;

  SplitNode();
  explicit SplitNode(PaneLeaf leaf);
  explicit SplitNode(SplitBranch branch);
  SplitNode(const SplitNode& other);
  SplitNode& operator=(const SplitNode& other);
  SplitNode(SplitNode&& other) noexcept;
  SplitNode& operator=(SplitNode&& other) noexcept;
  ~SplitNode();
};

std::string to_string(PaneKind value);
std::string to_string(TabRole value);
std::string to_string(SplitAxis value);
std::string to_string(TaskState value);

std::optional<PaneKind> parse_pane_kind(const std::string& value);
std::optional<TabRole> parse_tab_role(const std::string& value);
std::optional<SplitAxis> parse_split_axis(const std::string& value);

std::string serialize_anchor(const PaperAnchor& anchor);
std::optional<PaperAnchor> parse_anchor(const std::string& text);

}  // namespace deck
