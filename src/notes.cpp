#include "deck/notes.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <iomanip>
#include <sstream>

namespace deck {
namespace {

std::filesystem::path scratch_file_path(const std::filesystem::path& root) {
  return config_dir_for(root) / "scratch.md";
}

std::string note_context_directory(NoteContextKind kind) {
  switch (kind) {
    case NoteContextKind::File:
      return "files";
    case NoteContextKind::SearchResult:
      return "search";
    case NoteContextKind::Market:
      return "markets";
    case NoteContextKind::None:
      break;
  }
  return "general";
}

std::string sanitize_note_key(const std::string& key) {
  std::string result;
  result.reserve(key.size());
  for (unsigned char ch : key) {
    if (std::isalnum(ch)) {
      result.push_back(static_cast<char>(std::tolower(ch)));
    } else if (ch == '-' || ch == '_') {
      result.push_back(static_cast<char>(ch));
    } else {
      result.push_back('_');
    }
  }
  while (!result.empty() && result.back() == '_') result.pop_back();
  if (result.empty()) result = "note";
  if (result.size() > 48) result.resize(48);
  return result;
}

std::filesystem::path note_file_path(const std::filesystem::path& root, const NoteContext& context) {
  const auto hash_value = static_cast<unsigned long long>(std::hash<std::string>{}(context.key));
  std::ostringstream filename;
  filename << sanitize_note_key(context.label.empty() ? context.key : context.label) << "-" << std::hex
           << hash_value << ".md";
  return config_dir_for(root) / "notes" / note_context_directory(context.kind) / filename.str();
}

NoteContext market_note_context(const WorkspaceRuntimeState& runtime) {
  if (runtime.market_entries.empty() || runtime.selected_market_index >= runtime.market_entries.size()) return {};
  const auto& entry = runtime.market_entries[runtime.selected_market_index];
  return {NoteContextKind::Market, entry.symbol, "symbol-" + entry.symbol};
}

NoteContext file_note_context(const WorkspaceRuntimeState& runtime) {
  if (runtime.files_entries.empty() || runtime.selected_file_index >= runtime.files_entries.size()) return {};
  const auto& entry = runtime.files_entries[runtime.selected_file_index];
  if (entry.is_directory) return {};
  const auto full_path = (std::filesystem::path(runtime.files_browser_root) / entry.path).lexically_normal().string();
  return {NoteContextKind::File, full_path, full_path};
}

NoteContext search_note_context(const WorkspaceRuntimeState& runtime) {
  if (runtime.search_results.empty() || runtime.selected_search_result >= runtime.search_results.size()) return {};
  const auto& result = runtime.search_results[runtime.selected_search_result];
  return {NoteContextKind::SearchResult,
          result.path + ":" + std::to_string(result.line) + ":" + std::to_string(result.column),
          result.path + "-" + std::to_string(result.line)};
}

NoteContext desired_note_context(const WorkspaceRuntimeState& runtime) {
  if (!runtime.search_results.empty()) return search_note_context(runtime);
  if (!runtime.files_entries.empty()) return file_note_context(runtime);
  if (!runtime.market_entries.empty()) return market_note_context(runtime);
  return {};
}

bool switch_note_context(const std::filesystem::path& root,
                         WorkspaceRuntimeState& runtime,
                         const NoteContext& context) {
  if (context.kind == NoteContextKind::None || context.key.empty()) return false;
  if (runtime.note_editor.editing || runtime.note_editor.dirty) return false;
  if (runtime.note_context.kind == context.kind && runtime.note_context.key == context.key) return false;
  runtime.note_context = context;
  runtime.note_editor.buffer = load_context_note(root, context);
  runtime.note_editor.cursor = runtime.note_editor.buffer.size();
  runtime.note_editor.dirty = false;
  runtime.note_editor.editing = false;
  return true;
}

}  // namespace

std::string load_context_note(const std::filesystem::path& root, const NoteContext& context) {
  if (context.kind == NoteContextKind::None || context.key.empty()) return {};
  std::ifstream in(note_file_path(root, context));
  if (!in) return {};
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool save_context_note(const std::filesystem::path& root,
                       const NoteContext& context,
                       const std::string& text) {
  if (context.kind == NoteContextKind::None || context.key.empty()) return false;
  const auto path = note_file_path(root, context);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
  out << text;
  return static_cast<bool>(out);
}

std::string load_scratch_buffer(const std::filesystem::path& root) {
  std::ifstream in(scratch_file_path(root));
  if (!in) return {};
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool save_scratch_buffer(const std::filesystem::path& root, const std::string& text) {
  std::error_code ec;
  std::filesystem::create_directories(config_dir_for(root), ec);
  std::ofstream out(scratch_file_path(root), std::ios::trunc);
  if (!out) return false;
  out << text;
  return static_cast<bool>(out);
}

void refresh_note_context(const std::filesystem::path& root, WorkspaceRuntimeState& runtime) {
  switch_note_context(root, runtime, desired_note_context(runtime));
}

std::size_t line_start_for(const std::string& text, std::size_t cursor) {
  cursor = std::min(cursor, text.size());
  while (cursor > 0 && text[cursor - 1] != '\n') --cursor;
  return cursor;
}

std::size_t line_end_for(const std::string& text, std::size_t cursor) {
  cursor = std::min(cursor, text.size());
  while (cursor < text.size() && text[cursor] != '\n') ++cursor;
  return cursor;
}

std::size_t move_cursor_vertical(const std::string& text, std::size_t cursor, int direction) {
  const auto current_start = line_start_for(text, cursor);
  const auto current_end = line_end_for(text, cursor);
  const auto column = cursor - current_start;
  if (direction < 0) {
    if (current_start == 0) return cursor;
    const auto previous_end = current_start - 1;
    const auto previous_start = line_start_for(text, previous_end);
    return std::min(previous_start + column, previous_end);
  }
  if (current_end >= text.size()) return cursor;
  const auto next_start = current_end + 1;
  const auto next_end = line_end_for(text, next_start);
  return std::min(next_start + column, next_end);
}

}  // namespace deck
