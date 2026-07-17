#include "deck/news.h"

#include <cctype>

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
    if (entry.url.empty()) {
      const auto id = json_string(object, "objectID");
      if (!id.empty()) entry.url = "https://news.ycombinator.com/item?id=" + id;
    }
    if (!entry.title.empty() && !entry.url.empty()) entries.push_back(std::move(entry));
  }
  return entries;
}

}  // namespace deck
