#include "deck/workspace.h"

#include "deck/split_tree.h"

#include <sstream>

namespace deck {
namespace {

SplitNode dev_layout() {
  return make_split(
      SplitAxis::Horizontal,
      0.25,
      make_leaf(PaneKind::Files),
      make_split(
          SplitAxis::Vertical,
          0.70,
          make_leaf(PaneKind::Terminal),
          make_split(SplitAxis::Horizontal, 0.55, make_leaf(PaneKind::Search), make_leaf(PaneKind::Git))));
}

SplitNode run_layout() {
  return make_split(SplitAxis::Vertical,
                    0.66,
                    make_leaf(PaneKind::Terminal),
                    make_split(SplitAxis::Horizontal, 0.5, make_leaf(PaneKind::Tasks), make_leaf(PaneKind::Logs)));
}

SplitNode review_layout() {
  return make_split(SplitAxis::Horizontal,
                    0.24,
                    make_leaf(PaneKind::Files),
                    make_split(SplitAxis::Horizontal, 0.40, make_leaf(PaneKind::Git), make_leaf(PaneKind::Diff)));
}

SplitNode finance_layout() {
  return make_split(SplitAxis::Horizontal,
                    0.22,
                    make_leaf(PaneKind::Markets),
                    make_split(SplitAxis::Vertical,
                               0.72,
                               make_leaf(PaneKind::Portfolio),
                               make_leaf(PaneKind::Notes)));
}

SplitNode scratch_layout() {
  return make_split(SplitAxis::Vertical, 0.50, make_leaf(PaneKind::Notes), make_leaf(PaneKind::Scratch));
}

std::string escape(std::string value) {
  for (char& ch : value) {
    if (ch == '\n' || ch == '\r') {
      ch = ' ';
    }
  }
  return value;
}

bool is_supported_external_editor(const std::string& editor) {
  return editor == "vim" || editor == "nvim" || editor == "vscode";
}

}  // namespace

WorkspacePersistentState make_default_workspace(const std::filesystem::path& root) {
  WorkspacePersistentState state;
  state.name = root.filename().string().empty() ? "workspace" : root.filename().string();
  state.root = root;
  state.tabs = {
      TabPersistentState{"Dev", TabRole::Dev, dev_layout(), PaneKind::Terminal},
      TabPersistentState{"Run", TabRole::Run, run_layout(), PaneKind::Terminal},
      TabPersistentState{"Review", TabRole::Review, review_layout(), PaneKind::Git},
      TabPersistentState{"Finance", TabRole::Finance, finance_layout(), PaneKind::Markets},
      TabPersistentState{"Notes", TabRole::Notes, scratch_layout(), PaneKind::Scratch},
  };
  state.recent_commands = {"cmake --build build", "ctest --test-dir build", "git status --short"};
  return state;
}

void ensure_workspace_tabs(WorkspacePersistentState& state) {
  const auto defaults = make_default_workspace(state.root);
  for (const auto& default_tab : defaults.tabs) {
    const auto exists = std::any_of(state.tabs.begin(), state.tabs.end(), [&](const TabPersistentState& tab) {
      return tab.role == default_tab.role;
    });
    if (!exists) {
      state.tabs.push_back(default_tab);
    }
  }
  if (state.focused_tab >= state.tabs.size()) {
    state.focused_tab = 0;
  }
}

std::string serialize_workspace(const WorkspacePersistentState& state) {
  std::ostringstream out;
  out << "workspace_name=" << escape(state.name) << "\n";
  out << "workspace_root=" << escape(state.root.string()) << "\n";
  out << "external_editor=" << state.external_editor << "\n";
  out << "focused_tab=" << state.focused_tab << "\n";
  out << "selected_log_source=" << escape(state.selected_log_source) << "\n";
  if (state.last_anchor) {
    out << "last_anchor=" << serialize_anchor(*state.last_anchor) << "\n";
  }
  out << "recent_commands=";
  for (std::size_t i = 0; i < state.recent_commands.size(); ++i) {
    if (i != 0) {
      out << "||";
    }
    out << escape(state.recent_commands[i]);
  }
  out << "\n";
  for (const auto& tab : state.tabs) {
    out << "[[tab]]\n";
    out << "name=" << escape(tab.name) << "\n";
    out << "role=" << to_string(tab.role) << "\n";
    out << "focused_pane=" << to_string(tab.focused_pane) << "\n";
    out << "layout=" << serialize_split_tree(tab.layout) << "\n";
  }
  return out.str();
}

std::optional<WorkspacePersistentState> parse_workspace(
    const std::string& text, const std::filesystem::path& fallback_root) {
  WorkspacePersistentState state;
  state.root = fallback_root;
  std::istringstream in(text);
  std::string line;
  TabPersistentState* current_tab = nullptr;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    if (line == "[[tab]]") {
      state.tabs.emplace_back();
      current_tab = &state.tabs.back();
      continue;
    }
    auto pos = line.find('=');
    if (pos == std::string::npos) {
      continue;
    }
    auto key = line.substr(0, pos);
    auto value = line.substr(pos + 1);
    if (current_tab != nullptr) {
      if (key == "name") {
        current_tab->name = value;
      } else if (key == "role") {
        auto parsed = parse_tab_role(value);
        if (!parsed) {
          return std::nullopt;
        }
        current_tab->role = *parsed;
      } else if (key == "focused_pane") {
        auto parsed = parse_pane_kind(value);
        if (!parsed) {
          return std::nullopt;
        }
        current_tab->focused_pane = *parsed;
      } else if (key == "layout") {
        auto parsed = parse_split_tree(value);
        if (!parsed) {
          return std::nullopt;
        }
        current_tab->layout = std::move(*parsed);
      }
      continue;
    }
    if (key == "workspace_name") {
      state.name = value;
    } else if (key == "workspace_root" && !value.empty()) {
      state.root = value;
    } else if (key == "external_editor" && is_supported_external_editor(value)) {
      state.external_editor = value;
    } else if (key == "focused_tab") {
      state.focused_tab = static_cast<std::size_t>(std::stoul(value));
    } else if (key == "selected_log_source") {
      state.selected_log_source = value;
    } else if (key == "last_anchor") {
      state.last_anchor = parse_anchor(value);
    } else if (key == "recent_commands" && !value.empty()) {
      std::size_t cursor = 0;
      while (cursor <= value.size()) {
        auto next = value.find("||", cursor);
        auto piece = value.substr(cursor, next == std::string::npos ? std::string::npos : next - cursor);
        if (!piece.empty()) {
          state.recent_commands.push_back(piece);
        }
        if (next == std::string::npos) {
          break;
        }
        cursor = next + 2;
      }
    }
  }
  if (state.tabs.empty()) {
    return std::nullopt;
  }
  if (state.focused_tab >= state.tabs.size()) {
    state.focused_tab = 0;
  }
  if (state.name.empty()) {
    state.name = fallback_root.filename().string();
  }
  return state;
}

std::filesystem::path config_dir_for(const std::filesystem::path& root) { return root / ".deck"; }

std::filesystem::path state_file_for(const std::filesystem::path& root) {
  return config_dir_for(root) / "workspace.state";
}

std::filesystem::path database_file_for(const std::filesystem::path& root) {
  return config_dir_for(root) / "state.db";
}

}  // namespace deck
