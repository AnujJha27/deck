#pragma once

#include "deck/types.h"

#include <optional>
#include <string>
#include <vector>

namespace deck {

SplitNode make_leaf(PaneKind kind);
SplitNode make_split(SplitAxis axis, double ratio, SplitNode first, SplitNode second);

std::string serialize_split_tree(const SplitNode& node);
std::optional<SplitNode> parse_split_tree(const std::string& text);

bool resize_first_match(SplitNode& node, PaneKind target, double ratio);
bool resize_nearest_split(SplitNode& node, PaneKind target, SplitAxis axis, double delta);
bool contains_pane(const SplitNode& node, PaneKind target);
std::vector<PaneKind> pane_order(const SplitNode& node);

}  // namespace deck
