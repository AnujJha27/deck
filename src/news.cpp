#include "deck/news.h"

#include <cctype>
#include <algorithm>
#include <sstream>

namespace deck {
namespace {

std::string json_string(const std::string& object, const std::string& key) {
  const auto marker = "\"" + key + "\"";
  auto cursor = object.rfind(marker);
  if (cursor == std::string::npos) return {};
  cursor = object.find(':', cursor + marker.size());
  if (cursor == std::string::npos) return {};
  ++cursor;
  while (cursor < object.size() && std::isspace(static_cast<unsigned char>(object[cursor]))) ++cursor;
  if (object.compare(cursor, 4, "null") == 0 || cursor >= object.size() || object[cursor] != '"') return {};
  ++cursor;
  std::string value;
  bool escaping = false;
  while (cursor < object.size()) {
    const char ch = object[cursor++];
    if (escaping) {
      switch (ch) {
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        default: value.push_back(ch); break;
      }
      escaping = false;
    } else if (ch == '\\') {
      escaping = true;
    } else if (ch == '"') {
      break;
    } else {
      value.push_back(ch);
    }
  }
  return value;
}

std::vector<std::string> hit_objects(const std::string& json) {
  std::vector<std::string> objects;
  auto cursor = json.find("\"hits\"");
  cursor = cursor == std::string::npos ? cursor : json.find('[', cursor);
  if (cursor == std::string::npos) return objects;
  bool in_string = false;
  bool escaping = false;
  int depth = 0;
  std::size_t start = 0;
  for (++cursor; cursor < json.size(); ++cursor) {
    const char ch = json[cursor];
    if (escaping) {
      escaping = false;
      continue;
    }
    if (in_string && ch == '\\') {
      escaping = true;
      continue;
    }
    if (ch == '"') {
      in_string = !in_string;
      continue;
    }
    if (in_string) continue;
    if (ch == '{') {
      if (depth++ == 0) start = cursor;
    } else if (ch == '}' && depth > 0 && --depth == 0) {
      objects.push_back(json.substr(start, cursor - start + 1));
    } else if (ch == ']' && depth == 0) {
      break;
    }
  }
  return objects;
}

void replace_all(std::string& text, const std::string& from, const std::string& to) {
  for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
    text.replace(at, from.size(), to);
  }
}

std::string without_block(std::string html, const std::string& tag) {
  std::string lowered = html;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  const auto opening = "<" + tag;
  const auto closing = "</" + tag + ">";
  for (auto start = lowered.find(opening); start != std::string::npos; start = lowered.find(opening, start)) {
    auto end = lowered.find(closing, start);
    end = end == std::string::npos ? lowered.size() : end + closing.size();
    html.erase(start, end - start);
    lowered.erase(start, end - start);
  }
  return html;
}

}  // namespace

std::vector<NewsEntry> parse_hacker_news_response(const std::string& json,
                                                  const std::string& category) {
  std::vector<NewsEntry> entries;
  for (const auto& object : hit_objects(json)) {
    NewsEntry entry;
    entry.category = category;
    entry.title = json_string(object, "title");
    entry.url = json_string(object, "url");
    entry.source = json_string(object, "author");
    entry.published_at = json_string(object, "created_at");
    entry.summary = readable_article_text(json_string(object, "story_text"));
    if (entry.url.empty()) {
      const auto id = json_string(object, "objectID");
      if (!id.empty()) entry.url = "https://news.ycombinator.com/item?id=" + id;
    }
    if (!entry.title.empty() && !entry.url.empty()) entries.push_back(std::move(entry));
  }
  return entries;
}

std::string readable_article_text(const std::string& source, std::size_t limit) {
  auto html = without_block(without_block(source, "script"), "style");
  std::string text;
  text.reserve(std::min(html.size(), limit));
  bool in_tag = false;
  for (char ch : html) {
    if (ch == '<') {
      in_tag = true;
      if (!text.empty() && !std::isspace(static_cast<unsigned char>(text.back()))) text.push_back(' ');
    } else if (ch == '>') {
      in_tag = false;
    } else if (!in_tag) {
      text.push_back(ch);
    }
  }
  replace_all(text, "&amp;", "&");
  replace_all(text, "&lt;", "<");
  replace_all(text, "&gt;", ">");
  replace_all(text, "&quot;", "\"");
  replace_all(text, "&#x27;", "'");
  replace_all(text, "&#39;", "'");
  std::ostringstream cleaned;
  bool space = true;
  for (unsigned char ch : text) {
    if (std::isspace(ch)) {
      if (!space) cleaned << ' ';
      space = true;
    } else {
      cleaned << static_cast<char>(ch);
      space = false;
    }
    if (cleaned.tellp() >= static_cast<std::streampos>(limit)) break;
  }
  auto result = cleaned.str();
  while (!result.empty() && result.back() == ' ') result.pop_back();
  if (result.size() == limit) result += "…";
  return result;
}

bool safe_article_url(const std::string& url) {
  if (!url.starts_with("https://")) return false;
  const auto host_start = std::string("https://").size();
  const auto host_end = url.find_first_of("/:?#", host_start);
  auto host = url.substr(host_start, host_end - host_start);
  std::transform(host.begin(), host.end(), host.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (host.empty() || host.find('@') != std::string::npos || host == "localhost" ||
      host.ends_with(".localhost") || host.front() == '[') return false;
  if (host.starts_with("127.") || host.starts_with("10.") || host.starts_with("192.168.") ||
      host.starts_with("169.254.") || host == "0.0.0.0") return false;
  if (host.starts_with("172.")) {
    const auto second_end = host.find('.', 4);
    try {
      const auto second = std::stoi(host.substr(4, second_end - 4));
      if (second >= 16 && second <= 31) return false;
    } catch (...) {
      return false;
    }
  }
  return true;
}

}  // namespace deck
