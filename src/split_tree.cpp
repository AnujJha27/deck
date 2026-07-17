#include "deck/split_tree.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace deck {
namespace {

std::string trim(const std::string& input) {
  std::size_t start = 0;
  while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start]))) {
    ++start;
  }
  std::size_t end = input.size();
  while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    --end;
  }
  return input.substr(start, end - start);
}

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  std::optional<SplitNode> parse_node() {
    skip_ws();
    if (peek("leaf(")) {
      consume(5);
      auto token = read_until(')');
      if (!consume_char(')')) {
        return std::nullopt;
      }
      auto kind = parse_pane_kind(trim(token));
      if (!kind) {
        return std::nullopt;
      }
      return make_leaf(*kind);
    }
    if (peek("split(")) {
      consume(6);
      auto axis_token = trim(read_until(','));
      if (!consume_char(',')) {
        return std::nullopt;
      }
      auto ratio_token = trim(read_until(','));
      if (!consume_char(',')) {
        return std::nullopt;
      }
      auto axis = parse_split_axis(axis_token);
      if (!axis) {
        return std::nullopt;
      }
      double ratio = std::stod(ratio_token);
      auto first = parse_node();
      if (!first || !consume_char(',')) {
        return std::nullopt;
      }
      auto second = parse_node();
      if (!second || !consume_char(')')) {
        return std::nullopt;
      }
      return make_split(*axis, ratio, std::move(*first), std::move(*second));
    }
    return std::nullopt;
  }

  bool finished() {
    skip_ws();
    return pos_ == text_.size();
  }

 private:
  bool peek(std::string_view token) const { return text_.substr(pos_, token.size()) == token; }
  void consume(std::size_t count) { pos_ += count; }
  void skip_ws() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
    }
  }
  std::string read_until(char needle) {
    skip_ws();
    std::size_t start = pos_;
    int depth = 0;
    while (pos_ < text_.size()) {
      char current = text_[pos_];
      if (current == '(') {
        ++depth;
      } else if (current == ')') {
        if (depth == 0 && needle == ')') {
          break;
        }
        --depth;
      } else if (current == needle && depth == 0) {
        break;
      }
      ++pos_;
    }
    return std::string(text_.substr(start, pos_ - start));
  }
  bool consume_char(char ch) {
    skip_ws();
    if (pos_ >= text_.size() || text_[pos_] != ch) {
      return false;
    }
    ++pos_;
    return true;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

}  // namespace

SplitNode make_leaf(PaneKind kind) { return SplitNode{PaneLeaf{kind}}; }

SplitNode make_split(SplitAxis axis, double ratio, SplitNode first, SplitNode second) {
  ratio = std::clamp(ratio, 0.1, 0.9);
  SplitBranch branch{axis,
                     ratio,
                     std::make_unique<SplitNode>(std::move(first)),
                     std::make_unique<SplitNode>(std::move(second))};
  return SplitNode{std::move(branch)};
}

std::string serialize_split_tree(const SplitNode& node) {
  if (std::holds_alternative<PaneLeaf>(node.node)) {
    return "leaf(" + to_string(std::get<PaneLeaf>(node.node).kind) + ")";
  }
  const auto& branch = std::get<SplitBranch>(node.node);
  std::ostringstream out;
  out << "split(" << to_string(branch.axis) << "," << branch.ratio << ","
      << serialize_split_tree(*branch.first) << "," << serialize_split_tree(*branch.second) << ")";
  return out.str();
}

std::optional<SplitNode> parse_split_tree(const std::string& text) {
  Parser parser(text);
  auto node = parser.parse_node();
  if (!node || !parser.finished()) {
    return std::nullopt;
  }
  return node;
}

bool resize_first_match(SplitNode& node, PaneKind target, double ratio) {
  ratio = std::clamp(ratio, 0.1, 0.9);
  if (std::holds_alternative<PaneLeaf>(node.node)) {
    return std::get<PaneLeaf>(node.node).kind == target;
  }
  auto& branch = std::get<SplitBranch>(node.node);
  if (contains_pane(*branch.first, target)) {
    branch.ratio = ratio;
    return true;
  }
  return resize_first_match(*branch.first, target, ratio) ||
         resize_first_match(*branch.second, target, ratio);
}

bool resize_nearest_split(SplitNode& node, PaneKind target, SplitAxis axis, double delta) {
  if (std::holds_alternative<PaneLeaf>(node.node)) {
    return false;
  }
  auto& branch = std::get<SplitBranch>(node.node);
  SplitNode* child = nullptr;
  if (contains_pane(*branch.first, target)) {
    child = branch.first.get();
  } else if (contains_pane(*branch.second, target)) {
    child = branch.second.get();
  } else {
    return false;
  }
  if (resize_nearest_split(*child, target, axis, delta)) {
    return true;
  }
  if (branch.axis != axis) {
    return false;
  }
  branch.ratio = std::clamp(branch.ratio + delta, 0.1, 0.9);
  return true;
}

bool contains_pane(const SplitNode& node, PaneKind target) {
  if (std::holds_alternative<PaneLeaf>(node.node)) {
    return std::get<PaneLeaf>(node.node).kind == target;
  }
  const auto& branch = std::get<SplitBranch>(node.node);
  return contains_pane(*branch.first, target) || contains_pane(*branch.second, target);
}

std::vector<PaneKind> pane_order(const SplitNode& node) {
  std::vector<PaneKind> result;
  const auto visit = [&](const auto& self, const SplitNode& current) -> void {
    if (std::holds_alternative<PaneLeaf>(current.node)) {
      result.push_back(std::get<PaneLeaf>(current.node).kind);
      return;
    }
    const auto& branch = std::get<SplitBranch>(current.node);
    self(self, *branch.first);
    self(self, *branch.second);
  };
  visit(visit, node);
  return result;
}

}  // namespace deck
