#pragma once

#include "deck/workspace.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace deck {

std::string load_context_note(const std::filesystem::path& root, const NoteContext& context);
bool save_context_note(const std::filesystem::path& root,
                       const NoteContext& context,
                       const std::string& text);
std::string load_scratch_buffer(const std::filesystem::path& root);
bool save_scratch_buffer(const std::filesystem::path& root, const std::string& text);
void refresh_note_context(const std::filesystem::path& root, WorkspaceRuntimeState& runtime);

std::size_t line_start_for(const std::string& text, std::size_t cursor);
std::size_t line_end_for(const std::string& text, std::size_t cursor);
std::size_t move_cursor_vertical(const std::string& text, std::size_t cursor, int direction);

}  // namespace deck
