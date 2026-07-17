#include "deck/news.h"

#include <cctype>
#include <algorithm>
#include <sstream>
#include <string_view>

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

void append_break(std::string& text, std::size_t count = 1) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
  const auto existing = text.size() >= 2 && text.ends_with("\n\n") ? 2U :
                        (!text.empty() && text.back() == '\n' ? 1U : 0U);
  text.append(count > existing ? count - existing : 0, '\n');
}

std::string tag_name(std::string_view tag) {
  std::size_t cursor = 0;
  while (cursor < tag.size() && (std::isspace(static_cast<unsigned char>(tag[cursor])) || tag[cursor] == '/')) ++cursor;
  const auto start = cursor;
  while (cursor < tag.size() && std::isalnum(static_cast<unsigned char>(tag[cursor]))) ++cursor;
  std::string name(tag.substr(start, cursor - start));
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return name;
}

bool is_skipped_tag(const std::string& tag) {
  return tag == "script" || tag == "style" || tag == "head" || tag == "nav" || tag == "footer" ||
         tag == "header" || tag == "aside" || tag == "form" || tag == "svg" || tag == "noscript" ||
         tag == "iframe";
}

bool is_paragraph_tag(const std::string& tag) {
  return tag == "p" || tag == "div" || tag == "article" || tag == "section" || tag == "main" ||
         tag == "blockquote" || tag == "pre" || tag == "figure" || tag == "figcaption";
}

std::string decode_entities(std::string text) {
  replace_all(text, "&nbsp;", " ");
  replace_all(text, "&amp;", "&");
  replace_all(text, "&lt;", "<");
  replace_all(text, "&gt;", ">");
  replace_all(text, "&quot;", "\"");
  replace_all(text, "&#x27;", "'");
  replace_all(text, "&#39;", "'");
  return text;
}

std::string clean_reader_text(const std::string& text, std::size_t limit) {
  std::istringstream input(decode_entities(text));
  std::ostringstream output;
  std::string line;
  bool pending_blank = false;
  while (std::getline(input, line)) {
    std::ostringstream compact;
    bool space = true;
    for (unsigned char ch : line) {
      if (std::isspace(ch)) {
        if (!space) compact << ' ';
        space = true;
      } else {
        compact << static_cast<char>(ch);
        space = false;
      }
    }
    auto cleaned = compact.str();
    while (!cleaned.empty() && cleaned.back() == ' ') cleaned.pop_back();
    if (cleaned.empty()) {
      pending_blank = output.tellp() > 0;
      continue;
    }
    if (output.tellp() > 0) output << (pending_blank ? "\n\n" : "\n");
    output << cleaned;
    pending_blank = false;
    if (output.tellp() >= static_cast<std::streampos>(limit)) break;
  }
  auto result = output.str();
  while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back()))) result.pop_back();
  if (result.size() > limit) {
    result.resize(limit);
    result += "…";
  }
  return result;
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
  auto html = source;
  std::string text;
  text.reserve(std::min(html.size(), limit));
  std::size_t cursor = 0;
  int skipped_depth = 0;
  while (cursor < html.size()) {
    if (html[cursor] != '<') {
      if (skipped_depth == 0) text.push_back(html[cursor]);
      ++cursor;
      continue;
    }
    const auto end = html.find('>', cursor + 1);
    if (end == std::string::npos) break;
    const std::string_view raw_tag(html.data() + cursor + 1, end - cursor - 1);
    const bool closing = !raw_tag.empty() && raw_tag.front() == '/';
    const auto tag = tag_name(raw_tag);
    if (is_skipped_tag(tag)) {
      if (closing && skipped_depth > 0) --skipped_depth;
      else if (!closing) ++skipped_depth;
      cursor = end + 1;
      continue;
    }
    if (skipped_depth == 0) {
      if (tag == "br") {
        append_break(text);
      } else if (!closing && (tag == "h1" || tag == "h2" || tag == "h3" || tag == "h4")) {
        append_break(text, 2);
        text.append(tag == "h1" ? "# " : tag == "h2" ? "## " : "### ");
      } else if (!closing && tag == "li") {
        append_break(text);
        text += "• ";
      } else if (is_paragraph_tag(tag) || closing && (tag == "li" || tag == "h1" || tag == "h2" ||
                                                      tag == "h3" || tag == "h4")) {
        append_break(text, 2);
      }
    }
    cursor = end + 1;
  }
  return clean_reader_text(text, limit);
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
