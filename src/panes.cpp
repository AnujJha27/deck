#include "deck/panes.h"

#include "deck/environment.h"
#include "deck/process.h"
#include "deck/workspace.h"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace deck {
namespace {
using namespace ftxui;

struct FileScanSummary {
  std::vector<std::string> top_level_entries;
  std::vector<std::string> markdown_files;
  std::vector<std::string> log_files;
  std::vector<std::string> pdf_files;
  std::vector<std::pair<std::string, std::size_t>> extensions;
  std::size_t total_entries = 0;
};

struct SnapshotCacheEntry {
  PaneDataSnapshot snapshot;
  FileScanSummary files;
};

std::mutex snapshot_cache_mutex;
std::map<std::string, SnapshotCacheEntry> snapshot_cache;

std::string trim(std::string value) {
  auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

std::vector<std::string> split_lines(const std::string& text, std::size_t limit) {
  std::vector<std::string> lines;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line) && lines.size() < limit) {
    auto cleaned = trim(line);
    if (!cleaned.empty()) {
      lines.push_back(cleaned);
    }
  }
  return lines;
}

std::string summarize_list(const std::vector<std::string>& values, std::size_t limit = 3) {
  if (values.empty()) {
    return "none";
  }
  std::ostringstream out;
  for (std::size_t i = 0; i < values.size() && i < limit; ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << values[i];
  }
  if (values.size() > limit) {
    out << ", +" << (values.size() - limit) << " more";
  }
  return out.str();
}

std::string summarize_extensions(const std::vector<std::pair<std::string, std::size_t>>& extensions,
                                 std::size_t limit = 3) {
  if (extensions.empty()) {
    return "none";
  }
  std::ostringstream out;
  for (std::size_t i = 0; i < extensions.size() && i < limit; ++i) {
    if (i != 0) {
      out << ", ";
    }
    out << extensions[i].first << " x" << extensions[i].second;
  }
  return out.str();
}

bool should_skip_dir(const std::filesystem::path& path) {
  const auto name = path.filename().string();
  return name == ".git" || name == "build" || name == ".deck";
}

FileScanSummary scan_workspace_files(const std::filesystem::path& root) {
  FileScanSummary summary;
  std::error_code ec;
  if (!std::filesystem::exists(root, ec) || ec) {
    return summary;
  }

  std::map<std::string, std::size_t> extension_counts;

  for (const auto& entry : std::filesystem::directory_iterator(root, ec)) {
    if (ec) {
      break;
    }
    ++summary.total_entries;
    auto label = entry.path().filename().string();
    if (entry.is_directory(ec)) {
      label += "/";
    }
    if (summary.top_level_entries.size() < 6) {
      summary.top_level_entries.push_back(label);
    }
  }

  std::filesystem::recursive_directory_iterator it(
      root, std::filesystem::directory_options::skip_permission_denied, ec);
  const auto end = std::filesystem::recursive_directory_iterator();
  while (!ec && it != end) {
    const auto current = it->path();
    if (it->is_directory(ec) && should_skip_dir(current)) {
      it.disable_recursion_pending();
      ++it;
      continue;
    }
    if (!it->is_regular_file(ec)) {
      ++it;
      continue;
    }
    const auto relative = std::filesystem::relative(current, root, ec);
    const auto label = ec ? current.filename().string() : relative.string();
    ec.clear();
    auto extension = current.extension().string();
    if (extension.empty()) {
      extension = "<none>";
    }
    ++extension_counts[extension];
    if ((extension == ".md" || extension == ".markdown") && summary.markdown_files.size() < 5) {
      summary.markdown_files.push_back(label);
    }
    if ((extension == ".log" || extension == ".txt") && summary.log_files.size() < 5) {
      summary.log_files.push_back(label);
    }
    if (extension == ".pdf" && summary.pdf_files.size() < 5) {
      summary.pdf_files.push_back(label);
    }
    ++it;
  }

  for (const auto& [extension, count] : extension_counts) {
    summary.extensions.push_back({extension, count});
  }
  std::sort(summary.extensions.begin(),
            summary.extensions.end(),
            [](const auto& lhs, const auto& rhs) {
              if (lhs.second != rhs.second) {
                return lhs.second > rhs.second;
              }
              return lhs.first < rhs.first;
            });
  return summary;
}

std::optional<ProcessResult> run_command(const std::vector<std::string>& argv,
                                         const std::filesystem::path& cwd,
                                         std::chrono::milliseconds timeout = std::chrono::milliseconds(250)) {
  ProcessRunner runner;
  ProcessRequest request;
  request.argv = argv;
  request.cwd = cwd;
  request.timeout = timeout;
  auto result = runner.run(request);
  if (result.exit_code != 0 && trim(result.stdout_text).empty() && trim(result.stderr_text).empty()) {
    return std::nullopt;
  }
  return result;
}

std::vector<std::string> lines_for_files(const WorkspacePersistentState& state, const FileScanSummary& files) {
  return {
      "Root: " + state.root.string(),
      "Top level: " + summarize_list(files.top_level_entries, 4),
      "Files seen: " + std::to_string(files.total_entries),
      "Hot extensions: " + summarize_extensions(files.extensions),
  };
}

std::vector<std::string> lines_for_terminal(const WorkspacePersistentState& state,
                                            const WorkspaceRuntimeState& runtime) {
  if (!runtime.task_history.empty()) {
    const auto& task = runtime.task_history.back();
    return {
        "Active task state: " + to_string(runtime.active_task_state),
        "Last task: " + task.name + " [" + to_string(task.state) + "]",
        std::string("Task mode: ") + (task.use_pty ? "PTY" : "pipe"),
        "Command: " + task.command,
        "Started: " + task.started_at,
        "Finished: " + task.finished_at,
        "Exit code: " + std::to_string(task.exit_code),
    };
  }
  return {
      "Active task state: " + to_string(runtime.active_task_state),
      "Recent command: " + (state.recent_commands.empty() ? std::string("none") : state.recent_commands.front()),
      "Command slots: " + std::to_string(state.recent_commands.size()),
      "PTY runner: available for command tasks",
  };
}

const TaskRecord* selected_task_record(const WorkspaceRuntimeState& runtime) {
  if (runtime.task_history.empty()) {
    return nullptr;
  }
  const auto index = std::min(runtime.selected_task_index, runtime.task_history.size() - 1);
  return &runtime.task_history[index];
}

std::vector<std::string> lines_for_search(const FileScanSummary& files, const EnvironmentCapabilities& caps) {
  std::vector<std::string> lines = {
      caps.rg ? "rg available for workspace search" : "rg missing from PATH",
      "Search scope: " + std::to_string(files.total_entries) + " top-level entries",
  };
  lines.push_back("Common file types: " + summarize_extensions(files.extensions));
  lines.push_back("Markdown targets: " + summarize_list(files.markdown_files, 3));
  return lines;
}

std::string git_status_label(const GitStatusEntry& entry) {
  if (entry.index_status == "?" && entry.worktree_status == "?") {
    return "untracked";
  }
  if (entry.index_status == "!" && entry.worktree_status == "!") {
    return "ignored";
  }

  std::vector<std::string> parts;
  auto push_state = [&](const std::string& code, const std::string& scope) {
    if (code == " " || code == ".") {
      return;
    }
    if (code == "M") {
      parts.push_back(scope + " modified");
    } else if (code == "A") {
      parts.push_back(scope + " added");
    } else if (code == "D") {
      parts.push_back(scope + " deleted");
    } else if (code == "R") {
      parts.push_back(scope + " renamed");
    } else if (code == "C") {
      parts.push_back(scope + " copied");
    } else if (code == "U") {
      parts.push_back(scope + " unmerged");
    } else {
      parts.push_back(scope + " " + code);
    }
  };

  push_state(entry.index_status, "index");
  push_state(entry.worktree_status, "worktree");
  if (parts.empty()) {
    return "clean";
  }
  return summarize_list(parts, 3);
}

std::vector<GitStatusEntry> parse_git_status_entries_impl(const std::string& text) {
  std::vector<GitStatusEntry> entries;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.size() < 3 || line[0] == '#') {
      continue;
    }
    GitStatusEntry entry;
    entry.index_status.assign(1, line[0]);
    entry.worktree_status.assign(1, line[1]);
    auto payload = line.substr(3);
    auto rename_sep = payload.find(" -> ");
    if (rename_sep != std::string::npos) {
      entry.renamed = true;
      entry.original_path = payload.substr(0, rename_sep);
      entry.path = payload.substr(rename_sep + 4);
    } else {
      entry.path = payload;
    }
    if (!entry.path.empty()) {
      entries.push_back(std::move(entry));
    }
  }
  return entries;
}

std::vector<GitBranchEntry> parse_git_branch_entries_impl(const std::string& text) {
  std::vector<GitBranchEntry> entries;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.size() < 2 || line[1] != ' ') {
      continue;
    }
    const auto name = line.substr(2);
    if (name.empty()) {
      continue;
    }
    entries.push_back({name, line[0] == '*'});
  }
  return entries;
}

std::vector<DiffHunk> parse_diff_hunks_impl(const std::string& text) {
  std::vector<DiffHunk> hunks;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.starts_with("@@")) {
      continue;
    }

    DiffHunk hunk;
    hunk.header = line;

    auto parse_range = [](const std::string& token, int& start, int& count) {
      if (token.size() < 2) {
        return false;
      }
      auto range = token.substr(1);
      auto comma = range.find(',');
      try {
        if (comma == std::string::npos) {
          start = std::stoi(range);
          count = 1;
        } else {
          start = std::stoi(range.substr(0, comma));
          count = std::stoi(range.substr(comma + 1));
        }
      } catch (const std::exception&) {
        return false;
      }
      return true;
    };

    const auto second_at = line.find("@@", 2);
    const auto body = line.substr(2, second_at == std::string::npos ? std::string::npos : second_at - 2);
    std::istringstream body_stream(body);
    std::string old_token;
    std::string new_token;
    if (!(body_stream >> old_token >> new_token)) {
      continue;
    }
    if (!parse_range(old_token, hunk.old_start, hunk.old_count) ||
        !parse_range(new_token, hunk.new_start, hunk.new_count)) {
      continue;
    }
    hunks.push_back(std::move(hunk));
  }
  return hunks;
}

std::optional<std::string> build_patch_for_hunk_impl(const std::string& diff_text, std::size_t hunk_index) {
  std::istringstream in(diff_text);
  std::vector<std::string> prefix_lines;
  std::vector<std::string> hunk_lines;
  std::string line;
  std::size_t current_hunk = 0;
  bool seen_any_hunk = false;
  bool collecting = false;

  while (std::getline(in, line)) {
    if (line.starts_with("@@")) {
      if (collecting) {
        break;
      }
      seen_any_hunk = true;
      if (current_hunk == hunk_index) {
        collecting = true;
        hunk_lines.push_back(line);
      }
      ++current_hunk;
      continue;
    }

    if (collecting) {
      if (line.starts_with("diff --git ")) {
        break;
      }
      hunk_lines.push_back(line);
      continue;
    }

    if (!seen_any_hunk) {
      prefix_lines.push_back(line);
    }
  }

  if (hunk_lines.empty()) {
    return std::nullopt;
  }

  std::ostringstream patch;
  for (const auto& prefix_line : prefix_lines) {
    patch << prefix_line << '\n';
  }
  for (const auto& hunk_line : hunk_lines) {
    patch << hunk_line << '\n';
  }
  return patch.str();
}

std::vector<std::string> lines_for_git(const WorkspacePersistentState& state,
                                       const WorkspaceRuntimeState& runtime,
                                       const EnvironmentCapabilities& caps) {
  if (!caps.git) {
    return {"git missing from PATH", "Status and diff panes are disabled"};
  }
  if (runtime.git_status_text.empty()) {
    return {"git status unavailable", "Repository metadata could not be read"};
  }
  auto lines = split_lines(runtime.git_status_text, 3);
  if (lines.empty()) {
    lines.push_back("Working tree clean");
  }
  lines.push_back("enter refresh  s/u file  S/U hunk  c commit  :branch <name>");
  lines.push_back("branch: " + (runtime.current_git_branch.empty() ? std::string("detached/unknown")
                                                                  : runtime.current_git_branch) +
                  "  local branches: " + std::to_string(runtime.git_branches.size()));
  lines.push_back("changed files: " + std::to_string(runtime.git_entries.size()));
  if (!runtime.git_entries.empty()) {
    const auto begin = runtime.selected_git_index > 2 ? runtime.selected_git_index - 2 : 0;
    const auto end = std::min(begin + 5, runtime.git_entries.size());
    for (std::size_t i = begin; i < end; ++i) {
      const auto& entry = runtime.git_entries[i];
      const auto prefix = i == runtime.selected_git_index ? "> " : "  ";
      auto label = prefix + entry.path + " [" + git_status_label(entry) + "]";
      if (entry.renamed && !entry.original_path.empty()) {
        label += " <- " + entry.original_path;
      }
      lines.push_back(label);
    }
  } else {
    lines.push_back("No changed files");
  }
  return lines;
}

std::vector<std::string> lines_for_logs(const WorkspacePersistentState& state,
                                        const WorkspaceRuntimeState& runtime,
                                        const FileScanSummary& files) {
  if (const auto* task = selected_task_record(runtime)) {
    return {
        "Selected source: " + state.selected_log_source,
        "Task: " + task->name + (task->use_pty ? " [PTY]" : " [pipe]"),
        task->stdout_excerpt.empty() ? "stdout: none" : "stdout: " + task->stdout_excerpt,
        task->stderr_excerpt.empty() ? "stderr: none" : "stderr: " + task->stderr_excerpt,
    };
  }
  return {
      "Selected source: " + state.selected_log_source,
      "Known log files: " + summarize_list(files.log_files, 3),
      "Task history: " + std::to_string(runtime.task_history.size()) + " entries",
      "No task output captured yet",
  };
}

std::vector<std::string> lines_for_tasks(const WorkspaceRuntimeState& runtime) {
  std::vector<std::string> lines = {
      "Active task state: " + to_string(runtime.active_task_state),
      "history: " + std::to_string(runtime.task_history.size()) + " tasks",
      "controls: j/k move  enter rerun selected  R rerun latest  x cancel",
  };

  if (runtime.task_history.empty()) {
    lines.push_back("No tasks launched yet.");
    return lines;
  }

  const auto selected = std::min(runtime.selected_task_index, runtime.task_history.size() - 1);
  const auto begin = selected > 2 ? selected - 2 : 0;
  const auto end = std::min(begin + 5, runtime.task_history.size());
  for (std::size_t i = begin; i < end; ++i) {
    const auto& task = runtime.task_history[i];
    const auto prefix = i == selected ? "> " : "  ";
    auto line = prefix + task.name + " [" + to_string(task.state) + "]";
    if (task.cancelled) {
      line += " cancelled";
    } else if (task.timed_out) {
      line += " timed out";
    } else if (task.state == TaskState::Exited || task.state == TaskState::Failed) {
      line += " exit=" + std::to_string(task.exit_code);
    }
    lines.push_back(line);
    if (i == selected && lines.size() < 12) {
      lines.push_back("  " + task.command);
    }
  }
  return lines;
}

std::vector<std::string> lines_for_markets(const WorkspaceRuntimeState& runtime) {
  std::vector<std::string> lines = {
      "Watchlist entries: " + std::to_string(runtime.market_entries.size()),
      "enter focus  j/k move  a add  x refresh",
      runtime.market_entries.empty() ? "No watchlist configured" : "Selected: " + runtime.market_entries[runtime.selected_market_index].symbol,
  };
  if (runtime.market_data_enabled) {
    lines.push_back("Provider: " + runtime.market_data_provider);
  } else {
    lines.push_back("Provider: disabled");
  }
  lines.push_back("Alerts: " + std::to_string(runtime.alert_rules.size()) + "  triggered: " +
                  std::to_string(runtime.triggered_alerts.size()));
  if (!runtime.market_data_enabled) {
    lines.push_back("Hint: add local quote CSV data, or ensure `doctor` sees FINNHUB_API_KEY and curl.");
  }
  return lines;
}

std::vector<std::string> lines_for_portfolio(const WorkspaceRuntimeState& runtime) {
  std::vector<std::string> lines = {
      runtime.current_market_symbol.empty() ? "No market selected" : "Focus: " + runtime.current_market_symbol,
      "Sources: " + std::to_string(runtime.finance_data_sources.size()),
  };
  if (!runtime.portfolio_lines.empty()) {
    lines.insert(lines.end(), runtime.portfolio_lines.begin(), runtime.portfolio_lines.end());
  }
  return lines;
}

std::string note_context_label(const NoteContext& context) {
  switch (context.kind) {
    case NoteContextKind::File:
      return "file";
    case NoteContextKind::SearchResult:
      return "search";
    case NoteContextKind::Market:
      return "market";
    case NoteContextKind::None:
      break;
  }
  return "none";
}

std::vector<std::string> lines_for_notes(const WorkspacePersistentState& state,
                                         const WorkspaceRuntimeState& runtime,
                                         const FileScanSummary& files) {
  std::vector<std::string> lines = {
      "Stored markdown files: " + std::to_string(files.markdown_files.size()),
      "Current context: " + note_context_label(runtime.note_context),
      "controls: E edit  Ctrl+S save  Ctrl+R reload  Esc stop editing",
  };
  if (runtime.note_context.kind == NoteContextKind::None || runtime.note_context.key.empty()) {
    lines.push_back("No file, search result, or market symbol selected yet.");
    lines.push_back("Known notes in tree: " + summarize_list(files.markdown_files, 3));
    return lines;
  }

  lines.push_back("Target: " + runtime.note_context.key);
  lines.push_back(std::string("Mode: ") + (runtime.note_editor.editing ? "editing" : "preview"));
  lines.push_back(std::string("Status: ") + (runtime.note_editor.dirty ? "unsaved changes" : "saved"));
  if (runtime.note_editor.buffer.empty()) {
    lines.push_back(runtime.note_editor.editing ? "> " : "Context note is empty.");
    return lines;
  }

  std::istringstream in(runtime.note_editor.buffer);
  std::string line;
  std::size_t current_index = 0;
  while (std::getline(in, line) && lines.size() < 16) {
    const bool cursor_here = runtime.note_editor.editing && runtime.note_editor.cursor >= current_index &&
                             runtime.note_editor.cursor <= current_index + line.size();
    if (cursor_here) {
      const auto column = runtime.note_editor.cursor - current_index;
      auto decorated = line;
      decorated.insert(column, "|");
      lines.push_back("> " + decorated);
    } else {
      lines.push_back("  " + line);
    }
    current_index += line.size() + 1;
  }

  if (runtime.note_editor.editing && runtime.note_editor.cursor == runtime.note_editor.buffer.size() &&
      !runtime.note_editor.buffer.empty() && runtime.note_editor.buffer.back() == '\n' && lines.size() < 16) {
    lines.push_back("> |");
  }
  return lines;
}

std::vector<std::string> lines_for_scratch(const WorkspaceRuntimeState& runtime) {
  const auto& editor = runtime.scratch_editor;
  std::vector<std::string> lines = {
      std::string("mode: ") + (editor.editing ? "editing" : "preview"),
      std::string("status: ") + (editor.dirty ? "unsaved changes" : "saved"),
      "controls: e edit  Ctrl+S save  Ctrl+R reload  Esc stop editing",
  };

  if (editor.buffer.empty()) {
    lines.push_back(editor.editing ? "> " : "Scratchpad is empty.");
    return lines;
  }

  std::istringstream in(editor.buffer);
  std::string line;
  std::size_t current_index = 0;
  while (std::getline(in, line) && lines.size() < 16) {
    const bool cursor_here = editor.editing && editor.cursor >= current_index &&
                             editor.cursor <= current_index + line.size();
    if (cursor_here) {
      const auto column = editor.cursor - current_index;
      auto decorated = line;
      decorated.insert(column, "|");
      lines.push_back("> " + decorated);
    } else {
      lines.push_back("  " + line);
    }
    current_index += line.size() + 1;
  }

  if (editor.editing && editor.cursor == editor.buffer.size() && !editor.buffer.empty() &&
      editor.buffer.back() == '\n' && lines.size() < 16) {
    lines.push_back("> |");
  }
  return lines;
}

std::vector<std::string> lines_for_diff(const WorkspacePersistentState& state,
                                        const WorkspaceRuntimeState& runtime,
                                        const EnvironmentCapabilities& caps) {
  if (!caps.git) {
    return {"git missing from PATH", "No diff summary available"};
  }
  if (!runtime.git_entries.empty() && runtime.selected_git_index < runtime.git_entries.size()) {
    const auto& selected = runtime.git_entries[runtime.selected_git_index];
    const auto staged = selected.index_status != " " && selected.index_status != "?";
    auto lines = split_lines(runtime.diff_preview_text, 18);
    if (!lines.empty()) {
      lines.insert(lines.begin(), "selected: " + selected.path);
      lines.insert(lines.begin() + 1, staged ? "view: staged diff" : "view: unstaged diff");
      lines.insert(lines.begin() + 2,
                   "hunks: " + std::to_string(runtime.diff_hunks.size()) + "  [/] move  S/U apply  enter open");
      if (!runtime.diff_hunks.empty() && runtime.selected_diff_hunk < runtime.diff_hunks.size()) {
        const auto& hunk = runtime.diff_hunks[runtime.selected_diff_hunk];
        lines.insert(lines.begin() + 3,
                     "focus: hunk " + std::to_string(runtime.selected_diff_hunk + 1) + " at line " +
                         std::to_string(std::max(hunk.new_start, 1)));
      }
      for (auto& line : lines) {
        if (line.starts_with("@@")) {
          auto it = std::find_if(runtime.diff_hunks.begin(),
                                 runtime.diff_hunks.end(),
                                 [&](const DiffHunk& hunk) { return hunk.header == line; });
          if (it != runtime.diff_hunks.end()) {
            const auto index = static_cast<std::size_t>(std::distance(runtime.diff_hunks.begin(), it));
            line = (index == runtime.selected_diff_hunk ? "> " : "  ") + line;
          }
        }
      }
      return lines;
    }
  }
  if (runtime.diff_preview_text.empty()) {
    return {"git diff unavailable", "Repository metadata could not be read"};
  }
  auto lines = split_lines(runtime.diff_preview_text, 5);
  if (lines.empty()) {
    lines.push_back("No unstaged diff");
  }
  return lines;
}

class StaticPane final : public Pane {
 public:
  StaticPane(PaneKind id, std::string title, std::vector<std::string> lines, PaneStatus status)
      : id_(id), title_(std::move(title)), lines_(std::move(lines)), status_(status) {
    component_ = Renderer([this] {
      Elements rows;
      for (const auto& line : lines_) {
        rows.push_back(text(line));
      }
      if (rows.empty()) {
        rows.push_back(text("No data"));
      }
      return window(text(title_), vbox(std::move(rows)) | flex);
    });
  }

  PaneId id() const override { return id_; }
  Component component() override { return component_; }
  PaneStatus status() const override { return status_; }

 private:
  PaneKind id_;
  std::string title_;
  std::vector<std::string> lines_;
  PaneStatus status_;
  Component component_;
};

std::vector<std::string> lines_for_pane(PaneKind kind, const PaneDataSnapshot& snapshot) {
  switch (kind) {
    case PaneKind::Files:
      return snapshot.files_lines;
    case PaneKind::Terminal:
      return snapshot.terminal_lines;
    case PaneKind::Search:
      return snapshot.search_lines;
    case PaneKind::Git:
      return snapshot.git_lines;
    case PaneKind::Logs:
      return snapshot.logs_lines;
    case PaneKind::Tasks:
      return snapshot.tasks_lines;
    case PaneKind::Markets:
      return snapshot.markets_lines;
    case PaneKind::Portfolio:
      return snapshot.portfolio_lines;
    case PaneKind::Notes:
      return snapshot.notes_lines;
    case PaneKind::Scratch:
      return snapshot.scratch_lines;
    case PaneKind::Diff:
      return snapshot.diff_lines;
  }
  return {};
}

PaneStatus status_for_pane(PaneKind kind, const EnvironmentCapabilities& caps) {
  switch (kind) {
    case PaneKind::Search:
      return caps.rg ? PaneStatus::Ready : PaneStatus::Degraded;
    case PaneKind::Git:
      return caps.git ? PaneStatus::Ready : PaneStatus::Degraded;
    case PaneKind::Tasks:
      return PaneStatus::Ready;
    case PaneKind::Portfolio:
      return PaneStatus::Ready;
    case PaneKind::Scratch:
      return PaneStatus::Ready;
    default:
      return PaneStatus::Idle;
  }
}

}  // namespace

PaneDataSnapshot build_pane_data_snapshot(const WorkspacePersistentState& state,
                                          const WorkspaceRuntimeState& runtime,
                                          const EnvironmentCapabilities& caps) {
  const auto cache_key = state.root.string();
  {
    std::lock_guard<std::mutex> lock(snapshot_cache_mutex);
    auto found = snapshot_cache.find(cache_key);
    if (found != snapshot_cache.end()) {
      auto snapshot = found->second.snapshot;
      if (!runtime.files_entries.empty()) {
        snapshot.files_lines = {
            "Root: " + state.root.string(),
            "Browse: " + runtime.files_browser_root,
            "enter open  j/k move",
            "items: " + std::to_string(runtime.files_entries.size()),
        };
        for (std::size_t i = 0; i < runtime.files_entries.size() && i < 8; ++i) {
          const auto& entry = runtime.files_entries[i];
          const auto prefix = i == runtime.selected_file_index ? "> " : "  ";
          snapshot.files_lines.push_back(prefix + entry.path + (entry.is_directory ? "/" : ""));
        }
      }
      snapshot.terminal_lines = lines_for_terminal(state, runtime);
      if (!runtime.current_search_query.empty()) {
        snapshot.search_lines = {
            "query: " + runtime.current_search_query,
            runtime.search_in_progress ? "status: running" : "status: ready",
            "results: " + std::to_string(runtime.search_results.size()),
            "enter open  [/] move",
        };
        for (std::size_t i = 0; i < runtime.search_results.size() && i < 4; ++i) {
          const auto& result = runtime.search_results[i];
          const auto prefix = i == runtime.selected_search_result ? "> " : "  ";
          snapshot.search_lines.push_back(
              prefix + result.path + ":" + std::to_string(result.line) + " " + result.preview);
        }
      }
      snapshot.git_lines = lines_for_git(state, runtime, caps);
      snapshot.diff_lines = lines_for_diff(state, runtime, caps);
      snapshot.tasks_lines = lines_for_tasks(runtime);
      if (!runtime.market_entries.empty()) {
        snapshot.markets_lines = {
            "Watchlist entries: " + std::to_string(runtime.market_entries.size()),
            "enter focus  j/k move  a add  x refresh",
        };
        for (std::size_t i = 0; i < runtime.market_entries.size() && i < 8; ++i) {
          const auto& market = runtime.market_entries[i];
          const auto prefix = i == runtime.selected_market_index ? "> " : "  ";
          auto line = prefix + market.symbol;
          const auto has_alert =
              std::any_of(runtime.triggered_alerts.begin(),
                          runtime.triggered_alerts.end(),
                          [&](const TriggeredAlert& alert) { return alert.symbol == market.symbol; });
          if (has_alert) {
            line += "  !alert";
          }
          if (auto quote = runtime.market_quotes.find(market.symbol); quote != runtime.market_quotes.end() &&
                                                                 quote->second.has_data) {
            std::ostringstream detail;
            detail << std::fixed << std::setprecision(2) << "  " << quote->second.last_price << "  "
                   << quote->second.percent_change << "%";
            line += detail.str();
          } else if (!market.note.empty()) {
            line += "  " + market.note;
          }
          snapshot.markets_lines.push_back(line);
        }
      }
      if (!runtime.portfolio_lines.empty() || !runtime.current_market_symbol.empty()) {
        snapshot.portfolio_lines = lines_for_portfolio(runtime);
      }
      snapshot.scratch_lines = lines_for_scratch(runtime);
      snapshot.logs_lines = lines_for_logs(state, runtime, found->second.files);
      if (!runtime.status_message.empty()) {
        snapshot.logs_lines.push_back("status: " + runtime.status_message);
      }
      return snapshot;
    }
  }

  const auto files = scan_workspace_files(state.root);
  PaneDataSnapshot snapshot;
  snapshot.files_lines = lines_for_files(state, files);
  if (!runtime.files_entries.empty()) {
    snapshot.files_lines = {
        "Root: " + state.root.string(),
        "Browse: " + runtime.files_browser_root,
        "enter open  j/k move",
        "items: " + std::to_string(runtime.files_entries.size()),
    };
    for (std::size_t i = 0; i < runtime.files_entries.size() && i < 8; ++i) {
      const auto& entry = runtime.files_entries[i];
      const auto prefix = i == runtime.selected_file_index ? "> " : "  ";
      snapshot.files_lines.push_back(prefix + entry.path + (entry.is_directory ? "/" : ""));
    }
  }
  snapshot.terminal_lines = lines_for_terminal(state, runtime);
  snapshot.search_lines = lines_for_search(files, caps);
  if (!runtime.current_search_query.empty()) {
    snapshot.search_lines = {
        "query: " + runtime.current_search_query,
        runtime.search_in_progress ? "status: running" : "status: ready",
        "results: " + std::to_string(runtime.search_results.size()),
        "enter open  [/] move",
    };
    for (std::size_t i = 0; i < runtime.search_results.size() && i < 4; ++i) {
      const auto& result = runtime.search_results[i];
      const auto prefix = i == runtime.selected_search_result ? "> " : "  ";
      snapshot.search_lines.push_back(prefix + result.path + ":" + std::to_string(result.line) + " " + result.preview);
    }
  }
  snapshot.git_lines = lines_for_git(state, runtime, caps);
  snapshot.logs_lines = lines_for_logs(state, runtime, files);
  snapshot.tasks_lines = lines_for_tasks(runtime);
  snapshot.markets_lines = lines_for_markets(runtime);
  snapshot.portfolio_lines = lines_for_portfolio(runtime);
  if (!runtime.market_entries.empty()) {
    snapshot.markets_lines = {
        "Watchlist entries: " + std::to_string(runtime.market_entries.size()),
        "enter focus  j/k move  a add  x refresh",
    };
    for (std::size_t i = 0; i < runtime.market_entries.size() && i < 8; ++i) {
      const auto& market = runtime.market_entries[i];
      const auto prefix = i == runtime.selected_market_index ? "> " : "  ";
      auto line = prefix + market.symbol;
      const auto has_alert =
          std::any_of(runtime.triggered_alerts.begin(),
                      runtime.triggered_alerts.end(),
                      [&](const TriggeredAlert& alert) { return alert.symbol == market.symbol; });
      if (has_alert) {
        line += "  !alert";
      }
      if (auto quote = runtime.market_quotes.find(market.symbol); quote != runtime.market_quotes.end() &&
                                                               quote->second.has_data) {
        std::ostringstream detail;
        detail << std::fixed << std::setprecision(2) << "  " << quote->second.last_price << "  "
               << quote->second.percent_change << "%";
        line += detail.str();
      } else if (!market.note.empty()) {
        line += "  " + market.note;
      }
      snapshot.markets_lines.push_back(line);
    }
  }
  snapshot.notes_lines = lines_for_notes(state, runtime, files);
  snapshot.scratch_lines = lines_for_scratch(runtime);
  snapshot.diff_lines = lines_for_diff(state, runtime, caps);
  if (!runtime.status_message.empty()) {
    snapshot.logs_lines.push_back("status: " + runtime.status_message);
  }

  {
    std::lock_guard<std::mutex> lock(snapshot_cache_mutex);
    snapshot_cache[cache_key] = SnapshotCacheEntry{snapshot, files};
  }
  return snapshot;
}

void invalidate_pane_data_snapshot(const std::filesystem::path& root) {
  std::lock_guard<std::mutex> lock(snapshot_cache_mutex);
  snapshot_cache.erase(root.string());
}

std::vector<GitStatusEntry> parse_git_status_entries(const std::string& text) {
  return parse_git_status_entries_impl(text);
}

std::vector<GitBranchEntry> parse_git_branch_entries(const std::string& text) {
  return parse_git_branch_entries_impl(text);
}

std::vector<DiffHunk> parse_diff_hunks(const std::string& text) {
  return parse_diff_hunks_impl(text);
}

std::optional<std::string> build_patch_for_hunk(const std::string& diff_text, std::size_t hunk_index) {
  return build_patch_for_hunk_impl(diff_text, hunk_index);
}

std::unique_ptr<Pane> make_static_pane(PaneKind kind,
                                       const PaneDataSnapshot& snapshot,
                                       const WorkspacePersistentState& state,
                                       const WorkspaceRuntimeState& runtime,
                                       const EnvironmentCapabilities& caps) {
  return std::make_unique<StaticPane>(
      kind, to_string(kind), lines_for_pane(kind, snapshot), status_for_pane(kind, caps));
}

}  // namespace deck
