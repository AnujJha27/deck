#pragma once

#include "deck/environment.h"
#include "deck/pane.h"
#include "deck/process.h"
#include "deck/workspace.h"

#include <chrono>
#include <memory>
#include <optional>

namespace deck {

struct PaneDataSnapshot {
  std::vector<std::string> files_lines;
  std::vector<std::string> terminal_lines;
  std::vector<std::string> search_lines;
  std::vector<std::string> git_lines;
  std::vector<std::string> logs_lines;
  std::vector<std::string> tasks_lines;
  std::vector<std::string> markets_lines;
  std::vector<std::string> portfolio_lines;
  std::vector<std::string> notes_lines;
  std::vector<std::string> scratch_lines;
  std::vector<std::string> diff_lines;
  std::vector<std::string> news_topics_lines;
  std::vector<std::string> news_feed_lines;
  std::vector<std::string> news_preview_lines;
};

PaneDataSnapshot build_pane_data_snapshot(const WorkspacePersistentState& state,
                                          const WorkspaceRuntimeState& runtime,
                                          const EnvironmentCapabilities& caps);
void invalidate_pane_data_snapshot(const std::filesystem::path& root);
std::vector<GitStatusEntry> parse_git_status_entries(const std::string& text);
std::vector<GitBranchEntry> parse_git_branch_entries(const std::string& text);
std::vector<DiffHunk> parse_diff_hunks(const std::string& text);
std::optional<std::string> build_patch_for_hunk(const std::string& diff_text, std::size_t hunk_index);

std::unique_ptr<Pane> make_static_pane(PaneKind kind,
                                       const PaneDataSnapshot& snapshot,
                                       const WorkspacePersistentState& state,
                                       const WorkspaceRuntimeState& runtime,
                                       const EnvironmentCapabilities& caps);

}  // namespace deck
