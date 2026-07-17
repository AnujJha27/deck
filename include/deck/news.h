#pragma once

#include "deck/workspace.h"

#include <string>
#include <cstddef>
#include <vector>

namespace deck {

std::vector<NewsEntry> parse_hacker_news_response(const std::string& json,
                                                  const std::string& category);
std::string readable_article_text(const std::string& html, std::size_t limit = 2400);
bool safe_article_url(const std::string& url);

}  // namespace deck
