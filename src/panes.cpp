#include "deck/panes.h"

#include "deck/environment.h"
#include "deck/process.h"
#include "deck/workspace.h"

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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

std::vector<std::string> wrapped_lines(const std::string& text,
                                       std::size_t width = 72,
                                       std::size_t limit = 12) {
  std::vector<std::string> lines;
  std::istringstream paragraphs(text);
  for (std::string paragraph; std::getline(paragraphs, paragraph) && lines.size() < limit;) {
    paragraph = trim(paragraph);
    if (paragraph.empty()) {
      if (!lines.empty() && !lines.back().empty()) lines.push_back({});
      continue;
    }
    std::istringstream words(paragraph);
    std::string line;
    std::string word;
    while (words >> word && lines.size() < limit) {
      if (!line.empty() && line.size() + word.size() + 1 > width) {
        lines.push_back(std::move(line));
        line.clear();
      }
      if (!line.empty()) line.push_back(' ');
      line += word;
    }
    if (!line.empty() && lines.size() < limit) lines.push_back(std::move(line));
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

std::optional<std::string> pygments_lexer_for(const std::filesystem::path& path) {
  const auto extension = path.extension().string();
  if (extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".h" || extension == ".hpp") return "cpp";
  if (extension == ".py") return "python";
  if (extension == ".js" || extension == ".mjs") return "javascript";
  if (extension == ".ts") return "typescript";
  if (extension == ".json") return "json";
  if (extension == ".sh" || extension == ".bash") return "bash";
  if (extension == ".cmake") return "cmake";
  if (extension == ".md") return "markdown";
  return std::nullopt;
}

SyntaxStyle syntax_style_for(std::string_view token) {
  if (token.find("Comment") != std::string_view::npos) return SyntaxStyle::Comment;
  if (token.find("String") != std::string_view::npos) return SyntaxStyle::String;
  if (token.find("Number") != std::string_view::npos) return SyntaxStyle::Number;
  if (token.find("Keyword") != std::string_view::npos) return SyntaxStyle::Keyword;
  if (token.find("Name") != std::string_view::npos) return SyntaxStyle::Name;
  if (token.find("Operator") != std::string_view::npos || token.find("Punctuation") != std::string_view::npos) {
    return SyntaxStyle::Punctuation;
  }
  return SyntaxStyle::Plain;
}

std::string decode_pygments_repr(std::string_view value) {
  if (value.size() < 2) return {};
  std::string decoded;
  for (std::size_t index = 1; index + 1 < value.size(); ++index) {
    const auto ch = value[index];
    if (ch != '\\' || index + 2 >= value.size()) {
      decoded.push_back(ch);
      continue;
    }
    const auto escaped = value[++index];
    if (escaped == 'n') decoded.push_back('\n');
    else if (escaped == 'r') decoded.push_back('\r');
    else if (escaped == 't') decoded.push_back('\t');
    else decoded.push_back(escaped);
  }
  return decoded;
}

std::vector<std::vector<SyntaxSpan>> pygments_highlight(const std::filesystem::path& path,
                                                         const std::string& source) {
  const auto lexer = pygments_lexer_for(path);
  if (!lexer) return {};
  ProcessRequest request;
  request.argv = {"pygmentize", "-f", "raw", "-l", *lexer};
  request.stdin_text = source;
  request.timeout = std::chrono::milliseconds(1200);
  const auto result = ProcessRunner{}.run(request);
  if (result.exit_code != 0 || result.timed_out) return {};

  std::vector<std::vector<SyntaxSpan>> lines(1);
  std::istringstream raw(result.stdout_text);
  for (std::string raw_line; std::getline(raw, raw_line);) {
    const auto tab = raw_line.find('\t');
    if (tab == std::string::npos) continue;
    const auto style = syntax_style_for(raw_line.substr(0, tab));
    const auto token = decode_pygments_repr(raw_line.substr(tab + 1));
    std::size_t cursor = 0;
    while (cursor <= token.size()) {
      const auto newline = token.find('\n', cursor);
      const auto part = token.substr(cursor, newline - cursor);
      if (!part.empty()) lines.back().push_back({part, style});
      if (newline == std::string::npos) break;
      lines.push_back({});
      cursor = newline + 1;
    }
  }
  return lines;
}

void append_selected_file_preview(std::vector<std::string>& lines,
                                  std::vector<std::optional<std::vector<SyntaxSpan>>>& highlights,
                                  const WorkspacePersistentState& state,
                                  const WorkspaceRuntimeState& runtime) {
  if (runtime.files_entries.empty() || runtime.selected_file_index >= runtime.files_entries.size()) {
    return;
  }
  const auto& entry = runtime.files_entries[runtime.selected_file_index];
  if (entry.is_directory) {
    return;
  }
  const auto path = state.root / runtime.files_browser_root / entry.path;
  std::error_code ec;
  constexpr std::uintmax_t max_preview_bytes = 512 * 1024;
  constexpr std::size_t max_preview_lines = 4000;
  const auto file_size = std::filesystem::file_size(path, ec);
  if (ec || file_size > max_preview_bytes) {
    lines.push_back("Preview unavailable: file exceeds 512 KiB safety limit");
    highlights.push_back(std::nullopt);
    return;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    lines.push_back("Preview unavailable: cannot read " + entry.path);
    highlights.push_back(std::nullopt);
    return;
  }
  std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (content.find('\0') != std::string::npos) {
    lines.push_back("Preview unavailable: binary file");
    highlights.push_back(std::nullopt);
    return;
  }
  const auto syntax_lines = pygments_highlight(path, content);
  std::istringstream preview(content);
  std::string line;
  std::size_t line_count = 0;
  while (line_count < max_preview_lines && std::getline(preview, line)) {
    const auto highlighted = line_count < syntax_lines.size() && line.size() <= 480
                                 ? std::optional<std::vector<SyntaxSpan>>(syntax_lines[line_count])
                                 : std::nullopt;
    if (line.size() > 480) line = line.substr(0, 480) + "…";
    lines.push_back("  " + line);
    highlights.push_back(std::move(highlighted));
    ++line_count;
  }
  const auto preview_at = lines.end() - static_cast<std::ptrdiff_t>(line_count);
  lines.insert(preview_at, "Preview: " + entry.path + "  ·  " + std::to_string(file_size) +
                               " bytes  ·  PgUp/PgDn scroll");
  highlights.insert(highlights.end() - static_cast<std::ptrdiff_t>(line_count), std::nullopt);
  if (std::getline(preview, line)) {
    lines.push_back("Preview capped at 4,000 lines");
    highlights.push_back(std::nullopt);
  }
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
        "Dev: :run <command>  / search  r recent command",
    };
  }
  return {
      "Active task state: " + to_string(runtime.active_task_state),
      "Recent command: " + (state.recent_commands.empty() ? std::string("none") : state.recent_commands.front()),
      "Command slots: " + std::to_string(state.recent_commands.size()),
      "PTY runner: available for command tasks",
      "Dev: :run <command>  / search  r recent command",
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
  if (!runtime.git_recent_commits.empty()) {
    lines.push_back("Recent commits:");
    for (std::size_t i = 0; i < runtime.git_recent_commits.size() && i < 3; ++i) {
      lines.push_back("  " + runtime.git_recent_commits[i]);
    }
  }
  lines.push_back("changed files: " + std::to_string(runtime.git_entries.size()));
  if (!runtime.git_entries.empty()) {
    const auto begin = runtime.selected_git_index > 2 ? runtime.selected_git_index - 2 : 0;
    const auto end = std::min(begin + 5, runtime.git_entries.size());
    for (std::size_t i = begin; i < end; ++i) {
      const auto& entry = runtime.git_entries[i];
      const auto prefix = i == runtime.selected_git_index ? "> " : "  ";
      auto label = prefix + entry.path + " [" + git_status_label(entry) + "]";
      if (entry.worktree_status != " " || entry.index_status == "?") {
        label += "  [s stage]";
      }
      if (entry.index_status != " " && entry.index_status != "?") {
        label += "  [u unstage]";
      }
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
      "enter focus  j/k move  a add  d remove  x refresh",
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
    lines.push_back("Hint: add local OHLC CSV data, or ensure doctor sees TWELVE_DATA_API_KEY and curl.");
  }
  return lines;
}

std::vector<std::string> lines_for_portfolio(const WorkspaceRuntimeState& runtime) {
  const auto symbol = runtime.current_market_symbol;
  if (symbol.empty()) {
    return {"No market selected", "Choose a watchlist symbol to render daily candles."};
  }
  const auto found = runtime.market_candles.find(symbol);
  if (found == runtime.market_candles.end() || found->second.empty()) {
    return {
        symbol + "  ·  1D",
        "No candle history available.",
        "Set TWELVE_DATA_API_KEY or add an OHLC CSV with:",
        "symbol,date,open,high,low,close,volume",
        "Press x to refresh market data.",
    };
  }

  const auto terminal = ftxui::Terminal::Size();
  const auto chart_height = candle_chart_height_for_terminal_rows(terminal.dimy);
  const auto max_candles = candle_chart_capacity_for_terminal_columns(terminal.dimx);
  const auto& all_candles = found->second;
  const auto begin = all_candles.size() > max_candles ? all_candles.size() - max_candles : 0;
  std::vector<MarketCandle> candles(all_candles.begin() + static_cast<std::ptrdiff_t>(begin), all_candles.end());
  double observed_high = candles.front().high;
  double observed_low = candles.front().low;
  for (const auto& candle : candles) {
    observed_high = std::max(observed_high, candle.high);
    observed_low = std::min(observed_low, candle.low);
  }
  const auto observed_range = observed_high - observed_low;
  const auto scale = std::max(observed_range, std::max(std::abs(observed_high), 1.0) * 0.01);
  const auto padding = scale * 0.15;
  const auto chart_high = observed_high + padding;
  const auto chart_low = observed_low - padding;
  const auto range = chart_high - chart_low;
  const auto row_for = [&](double value) {
    const auto normalized = (chart_high - value) / range;
    return static_cast<std::size_t>(std::clamp(normalized * (chart_height - 1), 0.0, chart_height - 1.0));
  };

  std::vector<std::string> lines;
  const auto& last = candles.back();
  const auto quote = runtime.market_quotes.find(symbol);
  const auto has_quote = quote != runtime.market_quotes.end() && quote->second.has_data;
  const auto last_price = has_quote ? quote->second.last_price : last.close;
  const auto day_change = has_quote ? quote->second.change : last.close - last.open;
  const auto day_change_percent = has_quote && quote->second.percent_change != 0.0
                                      ? quote->second.percent_change
                                      : (last.open == 0.0 ? 0.0 : day_change * 100.0 / last.open);
  const auto number = [](double value, int precision = 2) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
  };
  std::vector<std::string> snapshot = {
      "MARKET SNAPSHOT",
      "Last     " + number(last_price),
      "Day      " + std::string(day_change >= 0.0 ? "+" : "") + number(day_change) + "  (" +
          (day_change_percent >= 0.0 ? "+" : "") + number(day_change_percent) + "%)",
      "Session  " + number(last.low) + " — " + number(last.high),
      "Period   " + number(observed_low) + " — " + number(observed_high),
      "Volume   " + number(last.volume, 0),
      "Candles  " + std::to_string(candles.size()) + " / " + std::to_string(all_candles.size()),
      "Source   " + (has_quote ? quote->second.provider : runtime.market_data_provider),
  };
  const auto alert = std::find_if(runtime.triggered_alerts.begin(), runtime.triggered_alerts.end(), [&](const auto& entry) {
    return entry.symbol == symbol;
  });
  if (alert != runtime.triggered_alerts.end()) {
    snapshot.push_back("Alert    " + alert->message.substr(0, 56));
  }
  while (snapshot.size() + 3 > chart_height && snapshot.size() > 5) snapshot.pop_back();
  snapshot.push_back({});
  snapshot.push_back("VOLUME PROFILE");
  const auto maximum_volume = std::max_element(candles.begin(), candles.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.volume < rhs.volume;
  })->volume;
  const auto volume_count = std::min<std::size_t>(chart_height > snapshot.size() ? chart_height - snapshot.size() : 0,
                                                   std::min<std::size_t>(candles.size(), 14));
  for (std::size_t offset = volume_count; offset > 0; --offset) {
    const auto& candle = candles[candles.size() - offset];
    const auto bar_width = maximum_volume <= 0.0
                               ? 0U
                               : static_cast<unsigned>(std::clamp(candle.volume * 18.0 / maximum_volume, 1.0, 18.0));
    std::string bar;
    for (unsigned index = 0; index < bar_width; ++index) bar += "▇";
    snapshot.push_back(candle.datetime + "  " + bar + " " + number(candle.volume, 0));
  }
  const auto finance_pane_width = std::max(terminal.dimx * 78 / 100, 0);
  const auto chart_column_width = std::max(finance_pane_width / 2 - 12, 24);
  const auto candle_stride = std::clamp(chart_column_width / static_cast<int>(candles.size()), 2, 4);
  std::ostringstream heading;
  heading << symbol << "  ·  1D  ·  " << candles.size() << " sessions   O " << std::fixed << std::setprecision(2)
          << last.open << "  H " << last.high << "  L " << last.low << "  C " << last.close;
  lines.push_back(heading.str());
  for (std::size_t row = 0; row < chart_height; ++row) {
    std::ostringstream chart_row;
    if (row == 0) {
      chart_row << std::fixed << std::setprecision(2) << std::setw(9) << chart_high << " ┤";
    } else if (row + 1 == chart_height) {
      chart_row << std::fixed << std::setprecision(2) << std::setw(9) << chart_low << " ┤";
    } else {
      chart_row << "          │";
    }
    for (const auto& candle : candles) {
      const auto high_row = row_for(candle.high);
      const auto low_row = row_for(candle.low);
      const auto open_row = row_for(candle.open);
      const auto close_row = row_for(candle.close);
      const auto body_top = std::min(open_row, close_row);
      const auto body_bottom = std::max(open_row, close_row);
      if (row >= body_top && row <= body_bottom) {
        chart_row << (candle.close >= candle.open ? "█" : "▓") << std::string(candle_stride - 1, ' ');
      } else if (row >= high_row && row <= low_row) {
        chart_row << "│" << std::string(candle_stride - 1, ' ');
      } else {
        chart_row << std::string(candle_stride, ' ');
      }
    }
    chart_row << "  │  ";
    if (row < snapshot.size()) chart_row << snapshot[row];
    lines.push_back(chart_row.str());
  }
  lines.push_back("          └" + std::string(candles.size() * candle_stride, '-'));
  lines.push_back("           " + candles.front().datetime + "  →  " + candles.back().datetime);
  lines.push_back("           █ up/open-close   ▓ down/open-close   │ wick");
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
  while (std::getline(in, line) && lines.size() < 250) {
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
      !runtime.note_editor.buffer.empty() && runtime.note_editor.buffer.back() == '\n' && lines.size() < 250) {
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
  while (std::getline(in, line) && lines.size() < 250) {
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
      editor.buffer.back() == '\n' && lines.size() < 250) {
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
    auto lines = split_lines(runtime.diff_preview_text, 250);
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

Color pane_accent(PaneKind kind) {
  switch (kind) {
    case PaneKind::Files:
    case PaneKind::Search:
      return Color::Cyan;
    case PaneKind::Terminal:
    case PaneKind::Tasks:
    case PaneKind::Logs:
      return Color::White;
    case PaneKind::Git:
    case PaneKind::Diff:
      return Color::Yellow;
    case PaneKind::Markets:
    case PaneKind::Portfolio:
      return Color::Green;
    case PaneKind::Notes:
    case PaneKind::Scratch:
      return Color::Cyan;
    case PaneKind::NewsTopics:
    case PaneKind::NewsFeed:
      return Color::Cyan;
    case PaneKind::NewsPreview:
      return Color::White;
  }
  return Color::White;
}

std::string pane_status_label(PaneStatus status) {
  switch (status) {
    case PaneStatus::Idle:
      return "idle";
    case PaneStatus::Busy:
      return "busy";
    case PaneStatus::Ready:
      return "ready";
    case PaneStatus::Degraded:
      return "limited";
  }
  return "unknown";
}

Element candle_row(const std::string& line) {
  constexpr std::string_view up = "█";
  constexpr std::string_view down = "▓";
  Elements segments;
  std::size_t cursor = 0;
  while (cursor < line.size()) {
    const auto up_pos = line.find(up, cursor);
    const auto down_pos = line.find(down, cursor);
    const auto next = std::min(up_pos, down_pos);
    if (next == std::string::npos) {
      segments.push_back(text(line.substr(cursor)));
      break;
    }
    if (next > cursor) {
      segments.push_back(text(line.substr(cursor, next - cursor)));
    }
    const auto rising = next == up_pos;
    const auto glyph = rising ? up : down;
    segments.push_back(text(std::string(glyph)) | color(rising ? Color::Green : Color::Yellow));
    cursor = next + glyph.size();
  }
  return hbox(std::move(segments));
}

Element styled_pane_row(PaneKind kind, const std::string& line) {
  if (kind == PaneKind::Portfolio && (line.find("█") != std::string::npos || line.find("▓") != std::string::npos)) {
    return candle_row(line);
  }
  auto row = text(line);
  if (line.starts_with("> ")) {
    return row | bold | color(Color::Cyan);
  }
  if (kind == PaneKind::Diff) {
    if (line.starts_with("+") && !line.starts_with("+++")) {
      return row | color(Color::Green);
    }
    if (line.starts_with("-") && !line.starts_with("---")) {
      return row | color(Color::Red);
    }
    if (line.starts_with("  @@") || line.starts_with("@@")) {
      return row | color(Color::Cyan);
    }
  }
  if (kind == PaneKind::Git) {
    if (line.find(" added]") != std::string::npos) {
      return row | color(Color::Green);
    }
    if (line.find(" deleted]") != std::string::npos) {
      return row | color(Color::Red);
    }
    if (line.find(" modified]") != std::string::npos || line.find("untracked") != std::string::npos) {
      return row | color(Color::Yellow);
    }
  }
  if (kind == PaneKind::Tasks || kind == PaneKind::Terminal || kind == PaneKind::Logs) {
    if (line.find("[failed]") != std::string::npos || line.find("exit=") != std::string::npos) {
      return row | color(Color::Red);
    }
    if (line.find("[exited]") != std::string::npos) {
      return row | color(Color::Green);
    }
    if (line.find("[running]") != std::string::npos || line.find("[starting]") != std::string::npos) {
      return row | color(Color::Cyan);
    }
  }
  if (line.starts_with("Preview:") || line.starts_with("selected:") || line.starts_with("focus:")) {
    return row | color(Color::Cyan);
  }
  if (line.starts_with("status:") || line.starts_with("controls:") || line.starts_with("enter ") ||
      line.starts_with("hunks:")) {
    return row | dim;
  }
  return row;
}

Element styled_syntax_line(const std::vector<SyntaxSpan>& spans) {
  Elements elements;
  elements.push_back(text("  "));
  for (const auto& span : spans) {
    auto element = text(span.text);
    switch (span.style) {
      case SyntaxStyle::Keyword:
        element = element | color(Color::Magenta);
        break;
      case SyntaxStyle::String:
        element = element | color(Color::Green);
        break;
      case SyntaxStyle::Comment:
        element = element | dim | color(Color::GrayDark);
        break;
      case SyntaxStyle::Number:
        element = element | color(Color::Yellow);
        break;
      case SyntaxStyle::Name:
        element = element | color(Color::Cyan);
        break;
      case SyntaxStyle::Punctuation:
        element = element | dim;
        break;
      case SyntaxStyle::Plain:
        break;
    }
    elements.push_back(std::move(element));
  }
  return hbox(std::move(elements));
}

class StaticPane final : public Pane {
 public:
  StaticPane(PaneKind id,
             std::string title,
             std::vector<std::string> lines,
             std::vector<std::optional<std::vector<SyntaxSpan>>> highlights,
             PaneStatus status,
             bool focused)
      : id_(id), title_(std::move(title)), lines_(std::move(lines)), highlights_(std::move(highlights)),
        status_(status), focused_(focused) {
    component_ = Renderer([this] {
      Elements rows;
      for (std::size_t index = 0; index < lines_.size(); ++index) {
        if (id_ == PaneKind::Files && index < highlights_.size() && highlights_[index]) {
          rows.push_back(styled_syntax_line(*highlights_[index]));
        } else {
          rows.push_back(styled_pane_row(id_, lines_[index]));
        }
      }
      if (rows.empty()) {
        rows.push_back(text("No data"));
      }
      const auto accent = pane_accent(id_);
      const auto badge_color = status_ == PaneStatus::Degraded ? Color::Red : accent;
      auto title = hbox({
          text(focused_ ? "◆ " : "· ") | color(focused_ ? Color::Cyan : accent),
          text(title_) | (focused_ ? bold : dim),
          filler(),
          text(pane_status_label(status_)) | dim | color(badge_color),
      });
      return window(title, vbox(std::move(rows)) | flex);
    });
  }

  PaneId id() const override { return id_; }
  Component component() override { return component_; }
  PaneStatus status() const override { return status_; }

 private:
  PaneKind id_;
  std::string title_;
  std::vector<std::string> lines_;
  std::vector<std::optional<std::vector<SyntaxSpan>>> highlights_;
  PaneStatus status_;
  bool focused_ = false;
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
    case PaneKind::NewsTopics:
      return snapshot.news_topics_lines;
    case PaneKind::NewsFeed:
      return snapshot.news_feed_lines;
    case PaneKind::NewsPreview:
      return snapshot.news_preview_lines;
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
    case PaneKind::NewsTopics:
    case PaneKind::NewsFeed:
    case PaneKind::NewsPreview:
      return caps.curl ? PaneStatus::Ready : PaneStatus::Degraded;
    default:
      return PaneStatus::Idle;
  }
}

}  // namespace

PaneDataSnapshot build_pane_data_snapshot(const WorkspacePersistentState& state,
                                          const WorkspaceRuntimeState& runtime,
                                          const EnvironmentCapabilities& caps) {
  const auto fill_news = [&](PaneDataSnapshot& snapshot) {
    static const std::vector<std::string> categories = {"AI World", "AI Research", "Web3 Security"};
    snapshot.news_topics_lines = {"[/] change topic", "x refresh manually", "No key or paid API"};
    for (std::size_t category = 0; category < categories.size(); ++category) {
      const auto count = std::count_if(runtime.news_entries.begin(), runtime.news_entries.end(), [&](const auto& entry) {
        return entry.category == categories[category];
      });
      snapshot.news_topics_lines.push_back(
          std::string(category == runtime.selected_news_category ? "> " : "  ") + categories[category] +
          "  " + std::to_string(count));
    }
    snapshot.news_feed_lines = {
        runtime.news_refresh_in_progress ? "status: refreshing" : "status: cached",
        "j/k select  Enter open  x refresh  PgUp/PgDn preview",
    };
    if (runtime.news_entries.empty()) snapshot.news_feed_lines.push_back("Press x to fetch recent headlines");
    for (std::size_t i = 0, shown = 0; i < runtime.news_entries.size() && shown < 9; ++i) {
      if (runtime.news_entries[i].category != categories[runtime.selected_news_category]) continue;
      snapshot.news_feed_lines.push_back(std::string(i == runtime.selected_news_index ? "> " : "  ") +
                                         runtime.news_entries[i].title);
      ++shown;
    }
    if (runtime.selected_news_index < runtime.news_entries.size()) {
      const auto& selected = runtime.news_entries[runtime.selected_news_index];
      snapshot.news_preview_lines = {selected.title,
                                     "from " + selected.source + "  ·  " + selected.published_at,
                                     selected.url};
      snapshot.news_preview_lines.push_back(runtime.news_preview_in_progress ? "status: fetching article preview" :
                                                                              "v fetch article text");
      auto preview = selected.summary;
      if (const auto cached = runtime.news_article_previews.find(selected.url);
          cached != runtime.news_article_previews.end()) {
        preview = cached->second;
      }
      if (!preview.empty()) {
        snapshot.news_preview_lines.push_back("Reader view");
        const auto lines = wrapped_lines(preview, 72, 200);
        const auto max_start = lines.size() > 1 ? lines.size() - 1 : 0;
        const auto start = std::min(runtime.news_preview_scroll, max_start);
        const auto end = std::min(start + std::size_t{14}, lines.size());
        snapshot.news_preview_lines.push_back(
            "lines " + std::to_string(lines.empty() ? 0 : start + 1) + "-" + std::to_string(end) +
            " / " + std::to_string(lines.size()) + "  PgUp/PgDn scroll");
        snapshot.news_preview_lines.insert(snapshot.news_preview_lines.end(),
                                           lines.begin() + static_cast<std::ptrdiff_t>(start),
                                           lines.begin() + static_cast<std::ptrdiff_t>(end));
      } else {
        snapshot.news_preview_lines.push_back("No Hacker News text; press v for a bounded external preview");
      }
    } else {
      snapshot.news_preview_lines = {"No headline selected"};
    }
  };
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
            "enter open  j/k move  / search  :run <command>",
            "items: " + std::to_string(runtime.files_entries.size()),
        };
        if (runtime.review_files_mode) {
          snapshot.files_lines.insert(snapshot.files_lines.begin() + 2, "Review Files mode: f returns to changed files");
        }
        for (std::size_t i = 0; i < runtime.files_entries.size() && i < 8; ++i) {
          const auto& entry = runtime.files_entries[i];
          const auto prefix = i == runtime.selected_file_index ? "> " : "  ";
          snapshot.files_lines.push_back(prefix + entry.path + (entry.is_directory ? "/" : ""));
        }
        snapshot.files_highlight_lines.assign(snapshot.files_lines.size(), std::nullopt);
        append_selected_file_preview(snapshot.files_lines, snapshot.files_highlight_lines, state, runtime);
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
            "enter focus  j/k move  a add  d remove  x refresh",
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
      snapshot.notes_lines = lines_for_notes(state, runtime, found->second.files);
      snapshot.scratch_lines = lines_for_scratch(runtime);
      snapshot.logs_lines = lines_for_logs(state, runtime, found->second.files);
      if (!runtime.status_message.empty()) {
        snapshot.logs_lines.push_back("status: " + runtime.status_message);
      }
      fill_news(snapshot);
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
        "enter open  j/k move  / search  :run <command>",
        "items: " + std::to_string(runtime.files_entries.size()),
    };
    if (runtime.review_files_mode) {
      snapshot.files_lines.insert(snapshot.files_lines.begin() + 2, "Review Files mode: f returns to changed files");
    }
    for (std::size_t i = 0; i < runtime.files_entries.size() && i < 8; ++i) {
      const auto& entry = runtime.files_entries[i];
      const auto prefix = i == runtime.selected_file_index ? "> " : "  ";
      snapshot.files_lines.push_back(prefix + entry.path + (entry.is_directory ? "/" : ""));
    }
    snapshot.files_highlight_lines.assign(snapshot.files_lines.size(), std::nullopt);
    append_selected_file_preview(snapshot.files_lines, snapshot.files_highlight_lines, state, runtime);
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
        "enter focus  j/k move  a add  d remove  x refresh",
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
  fill_news(snapshot);

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

std::size_t candle_chart_height_for_terminal_rows(int terminal_rows) {
  constexpr std::size_t min_height = 10;
  constexpr std::size_t max_height = 42;
  const auto shell_rows = std::max(terminal_rows - 7, 0);
  const auto chart_pane_rows = static_cast<std::size_t>(shell_rows * 72 / 100);
  return std::clamp(chart_pane_rows > 6 ? chart_pane_rows - 6 : std::size_t{0}, min_height, max_height);
}

std::size_t candle_chart_capacity_for_terminal_columns(int terminal_columns) {
  constexpr std::size_t min_candles = 12;
  constexpr std::size_t max_candles = 60;
  const auto finance_pane_width = std::max(terminal_columns * 78 / 100, 0);
  const auto chart_column_width = std::max(finance_pane_width / 2 - 14, 0);
  const auto capacity = static_cast<std::size_t>(chart_column_width / 2);
  return std::clamp(capacity, min_candles, max_candles);
}

std::unique_ptr<Pane> make_static_pane(PaneKind kind,
                                       const PaneDataSnapshot& snapshot,
                                       const WorkspacePersistentState& state,
                                       const WorkspaceRuntimeState& runtime,
                                       const EnvironmentCapabilities& caps) {
  auto title = kind == PaneKind::Portfolio ? std::string("Chart") : to_string(kind);
  const bool focused = runtime.visible_tab < state.tabs.size() &&
                       state.tabs[runtime.visible_tab].focused_pane == kind;
  auto lines = lines_for_pane(kind, snapshot);
  auto highlights = kind == PaneKind::Files ? snapshot.files_highlight_lines
                                            : std::vector<std::optional<std::vector<SyntaxSpan>>>{};
  if (kind != PaneKind::NewsPreview) {
    const auto found = runtime.pane_scroll_offsets.find(kind);
    const auto requested = found == runtime.pane_scroll_offsets.end() ? 0 : found->second;
    const auto start = std::min(requested, lines.size() > 1 ? lines.size() - 1 : std::size_t{0});
    if (start > 0) {
      lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(start));
      if (start < highlights.size()) {
        highlights.erase(highlights.begin(), highlights.begin() + static_cast<std::ptrdiff_t>(start));
      } else {
        highlights.clear();
      }
      title += " · line " + std::to_string(start + 1);
    }
  }
  return std::make_unique<StaticPane>(
      kind, title, std::move(lines), std::move(highlights), status_for_pane(kind, caps), focused);
}

}  // namespace deck
