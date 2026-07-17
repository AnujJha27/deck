#pragma once

#include "deck/workspace.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace deck {

struct PaperRecord {
  std::string paper_id;
  std::filesystem::path canonical_path;
  std::uintmax_t file_size = 0;
  std::int64_t mtime = 0;
  std::string title;
  std::string reading_status = "unread";
  std::optional<std::int64_t> last_opened_unix_ms;
  std::optional<PaperAnchor> last_anchor;
  std::optional<std::filesystem::path> linked_note_path;
  std::vector<std::string> tags;
  std::vector<PaperAnchor> bookmarks;
};

class WorkspaceStore {
 public:
  std::optional<WorkspacePersistentState> load(const std::filesystem::path& root) const;
  bool save(const WorkspacePersistentState& state) const;
  bool reset_layout(const std::filesystem::path& root) const;
  std::vector<PaperRecord> load_papers(const std::filesystem::path& root) const;
  bool save_paper(const std::filesystem::path& root, const PaperRecord& paper) const;
  std::vector<TaskRecord> load_tasks(const std::filesystem::path& root) const;
  bool save_tasks(const std::filesystem::path& root, const std::vector<TaskRecord>& tasks) const;
  bool clear_tasks(const std::filesystem::path& root) const;

 private:
  bool ensure_schema(void* db_handle) const;
};

std::string redact_sensitive_text(std::string text);
bool task_argv_is_sensitive(const std::vector<std::string>& argv);

}  // namespace deck
