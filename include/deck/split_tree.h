#pragma once

#include "deck/types.h"

#include <optional>
#include <string>

namespace deck {

SplitNode make_leaf(PaneKind kind);
SplitNode make_split(SplitAxis axis, double ratio, SplitNode first, SplitNode second);

std::string serialize_split_tree(const SplitNode& node);
std::optional<SplitNode> parse_split_tree(const std::string& text);

bool resize_first_match(SplitNode& node, PaneKind target, double ratio);
bool contains_pane(const SplitNode& node, PaneKind target);

}  // namespace deck
