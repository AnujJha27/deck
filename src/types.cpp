#include "deck/types.h"

#include <charconv>
#include <sstream>

namespace deck {
namespace {

template <typename Enum>
std::optional<Enum> match_enum(const std::string& value,
                               std::initializer_list<std::pair<std::string_view, Enum>> entries) {
  for (const auto& [name, entry] : entries) {
    if (value == name) {
      return entry;
    }
  }
  return std::nullopt;
}

std::string optional_text(const std::optional<std::string>& value) {
  return value ? *value : "";
}

std::optional<double> parse_optional_double(const std::string& token) {
  if (token.empty() || token == "-") {
    return std::nullopt;
  }
  return std::stod(token);
}

std::optional<std::string> parse_optional_string(const std::string& token) {
  if (token.empty() || token == "-") {
    return std::nullopt;
  }
  return token;
}

}  // namespace

SplitBranch::SplitBranch() = default;

SplitBranch::SplitBranch(SplitAxis axis,
                         double ratio,
                         std::unique_ptr<SplitNode> first,
                         std::unique_ptr<SplitNode> second)
    : axis(axis), ratio(ratio), first(std::move(first)), second(std::move(second)) {}

SplitBranch::SplitBranch(const SplitBranch& other)
    : axis(other.axis),
      ratio(other.ratio),
      first(other.first ? std::make_unique<SplitNode>(*other.first) : nullptr),
      second(other.second ? std::make_unique<SplitNode>(*other.second) : nullptr) {}

SplitBranch& SplitBranch::operator=(const SplitBranch& other) {
  if (this == &other) {
    return *this;
  }
  axis = other.axis;
  ratio = other.ratio;
  first = other.first ? std::make_unique<SplitNode>(*other.first) : nullptr;
  second = other.second ? std::make_unique<SplitNode>(*other.second) : nullptr;
  return *this;
}

SplitBranch::SplitBranch(SplitBranch&& other) noexcept = default;
SplitBranch& SplitBranch::operator=(SplitBranch&& other) noexcept = default;
SplitBranch::~SplitBranch() = default;

SplitNode::SplitNode() : node(PaneLeaf{}) {}
SplitNode::SplitNode(PaneLeaf leaf) : node(std::move(leaf)) {}
SplitNode::SplitNode(SplitBranch branch) : node(std::move(branch)) {}
SplitNode::SplitNode(const SplitNode& other) : node(other.node) {}
SplitNode& SplitNode::operator=(const SplitNode& other) {
  node = other.node;
  return *this;
}
SplitNode::SplitNode(SplitNode&& other) noexcept = default;
SplitNode& SplitNode::operator=(SplitNode&& other) noexcept = default;
SplitNode::~SplitNode() = default;

std::string to_string(PaneKind value) {
  switch (value) {
    case PaneKind::Files:
      return "files";
    case PaneKind::Terminal:
      return "terminal";
    case PaneKind::Search:
      return "search";
    case PaneKind::Git:
      return "git";
    case PaneKind::Logs:
      return "logs";
    case PaneKind::Tasks:
      return "tasks";
    case PaneKind::Markets:
      return "markets";
    case PaneKind::Portfolio:
      return "portfolio";
    case PaneKind::Notes:
      return "notes";
    case PaneKind::Scratch:
      return "scratch";
    case PaneKind::Diff:
      return "diff";
    case PaneKind::MathInput:
      return "math-input";
    case PaneKind::MathResult:
      return "math-result";
    case PaneKind::MathPlot:
      return "math-plot";
    case PaneKind::NewsTopics:
      return "news-topics";
    case PaneKind::NewsFeed:
      return "news-feed";
    case PaneKind::NewsPreview:
      return "news-preview";
  }
  return "unknown";
}

std::string to_string(TabRole value) {
  switch (value) {
    case TabRole::Dev:
      return "dev";
    case TabRole::Review:
      return "review";
    case TabRole::Run:
      return "run";
    case TabRole::Finance:
      return "finance";
    case TabRole::Notes:
      return "notes";
    case TabRole::Math:
      return "math";
    case TabRole::News:
      return "news";
  }
  return "unknown";
}

std::string to_string(SplitAxis value) {
  switch (value) {
    case SplitAxis::Horizontal:
      return "horizontal";
    case SplitAxis::Vertical:
      return "vertical";
  }
  return "unknown";
}

std::string to_string(TaskState value) {
  switch (value) {
    case TaskState::Idle:
      return "idle";
    case TaskState::Starting:
      return "starting";
    case TaskState::Running:
      return "running";
    case TaskState::Exited:
      return "exited";
    case TaskState::Failed:
      return "failed";
    case TaskState::Cancelled:
      return "cancelled";
  }
  return "unknown";
}

std::optional<PaneKind> parse_pane_kind(const std::string& value) {
  return match_enum<PaneKind>(value,
                              {{"files", PaneKind::Files},
                               {"terminal", PaneKind::Terminal},
                               {"search", PaneKind::Search},
                               {"git", PaneKind::Git},
                               {"logs", PaneKind::Logs},
                               {"tasks", PaneKind::Tasks},
                               {"markets", PaneKind::Markets},
                               {"portfolio", PaneKind::Portfolio},
                               {"notes", PaneKind::Notes},
                               {"scratch", PaneKind::Scratch},
                               {"diff", PaneKind::Diff},
                               {"math-input", PaneKind::MathInput},
                               {"math-result", PaneKind::MathResult},
                               {"math-plot", PaneKind::MathPlot},
                               {"news-topics", PaneKind::NewsTopics},
                               {"news-feed", PaneKind::NewsFeed},
                               {"news-preview", PaneKind::NewsPreview}});
}

std::optional<TabRole> parse_tab_role(const std::string& value) {
  return match_enum<TabRole>(value,
                             {{"dev", TabRole::Dev},
                              {"review", TabRole::Review},
                              {"run", TabRole::Run},
                              {"finance", TabRole::Finance},
                              {"notes", TabRole::Notes},
                              {"math", TabRole::Math},
                              {"news", TabRole::News}});
}

std::optional<SplitAxis> parse_split_axis(const std::string& value) {
  return match_enum<SplitAxis>(value,
                               {{"horizontal", SplitAxis::Horizontal},
                                {"vertical", SplitAxis::Vertical}});
}

std::string serialize_anchor(const PaperAnchor& anchor) {
  std::ostringstream out;
  out << anchor.page << "|";
  out << (anchor.normalized_y ? std::to_string(*anchor.normalized_y) : "-") << "|";
  out << (anchor.text_quote ? *anchor.text_quote : "-") << "|";
  out << (anchor.section_hint ? *anchor.section_hint : "-");
  return out.str();
}

std::optional<PaperAnchor> parse_anchor(const std::string& text) {
  std::istringstream in(text);
  std::string page_token;
  std::string y_token;
  std::string quote_token;
  std::string section_token;
  if (!std::getline(in, page_token, '|') || !std::getline(in, y_token, '|') ||
      !std::getline(in, quote_token, '|') || !std::getline(in, section_token)) {
    return std::nullopt;
  }
  PaperAnchor anchor;
  anchor.page = std::stoi(page_token);
  anchor.normalized_y = parse_optional_double(y_token);
  anchor.text_quote = parse_optional_string(quote_token);
  anchor.section_hint = parse_optional_string(section_token);
  return anchor;
}

}  // namespace deck
