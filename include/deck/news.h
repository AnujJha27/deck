#pragma once

#include "deck/workspace.h"

#include <string>
#include <vector>

namespace deck {

std::vector<NewsEntry> parse_hacker_news_response(const std::string& json,
                                                  const std::string& category);

}  // namespace deck
