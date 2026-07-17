#include "deck/persistence.h"
#include "deck/split_tree.h"

#include <sqlite3.h>

#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <regex>
#include <string>

namespace deck {
namespace {

class SqliteDb {
 public:
  explicit SqliteDb(const std::filesystem::path& path) { sqlite3_open(path.string().c_str(), &db_); }
  ~SqliteDb() {
    if (db_ != nullptr) {
      sqlite3_close(db_);
    }
  }

  sqlite3* get() const { return db_; }
  explicit operator bool() const { return db_ != nullptr; }

 private:
  sqlite3* db_ = nullptr;
};

bool exec(sqlite3* db, const char* sql) { return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK; }

bool bind_text(sqlite3_stmt* stmt, int index, const std::string& value) {
  return sqlite3_bind_text(stmt, index, value.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

std::optional<std::string> column_text(sqlite3_stmt* stmt, int index) {
  const auto* value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
  if (value == nullptr) {
    return std::nullopt;
  }
  return std::string(value);
}

bool workspace_has_editor_column(sqlite3* db) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA table_info(workspace_ui_state)", -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  bool found = false;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (name != nullptr && std::string(name) == "external_editor") {
      found = true;
      break;
    }
  }
  sqlite3_finalize(stmt);
  return found;
}

}  // namespace

bool WorkspaceStore::ensure_schema(void* db_handle) const {
  auto* db = static_cast<sqlite3*>(db_handle);
  if (!exec(db,
              "BEGIN;"
              "CREATE TABLE IF NOT EXISTS schema_migrations(version INTEGER PRIMARY KEY);"
              "INSERT OR IGNORE INTO schema_migrations(version) VALUES(1);"
              "CREATE TABLE IF NOT EXISTS workspace_ui_state("
              "  workspace_root TEXT PRIMARY KEY,"
              "  workspace_name TEXT NOT NULL,"
              "  focused_tab INTEGER NOT NULL,"
              "  selected_log_source TEXT NOT NULL,"
              "  external_editor TEXT NOT NULL DEFAULT 'nvim',"
              "  last_anchor TEXT"
              ");"
              "CREATE TABLE IF NOT EXISTS workspace_tabs("
              "  workspace_root TEXT NOT NULL,"
              "  tab_order INTEGER NOT NULL,"
              "  name TEXT NOT NULL,"
              "  role TEXT NOT NULL,"
              "  focused_pane TEXT NOT NULL,"
              "  layout TEXT NOT NULL,"
              "  PRIMARY KEY(workspace_root, tab_order)"
              ");"
              "CREATE TABLE IF NOT EXISTS recent_commands("
              "  workspace_root TEXT NOT NULL,"
              "  command_order INTEGER NOT NULL,"
              "  command_text TEXT NOT NULL,"
              "  PRIMARY KEY(workspace_root, command_order)"
              ");"
              "CREATE TABLE IF NOT EXISTS task_history("
              "  workspace_root TEXT NOT NULL, task_order INTEGER NOT NULL, name TEXT NOT NULL,"
              "  command_text TEXT NOT NULL, use_pty INTEGER NOT NULL, state TEXT NOT NULL,"
              "  exit_code INTEGER NOT NULL, started_at TEXT NOT NULL, finished_at TEXT NOT NULL,"
              "  stdout_excerpt TEXT NOT NULL, stderr_excerpt TEXT NOT NULL,"
              "  timed_out INTEGER NOT NULL, cancelled INTEGER NOT NULL,"
              "  PRIMARY KEY(workspace_root, task_order)"
              ");"
              "CREATE TABLE IF NOT EXISTS task_argv("
              "  workspace_root TEXT NOT NULL, task_order INTEGER NOT NULL, arg_order INTEGER NOT NULL,"
              "  arg_text TEXT NOT NULL, PRIMARY KEY(workspace_root, task_order, arg_order)"
              ");"
              "CREATE TABLE IF NOT EXISTS papers("
              "  workspace_root TEXT NOT NULL,"
              "  paper_id TEXT NOT NULL,"
              "  canonical_path TEXT NOT NULL,"
              "  file_size INTEGER NOT NULL,"
              "  mtime INTEGER NOT NULL,"
              "  title TEXT NOT NULL,"
              "  reading_status TEXT NOT NULL,"
              "  last_opened_unix_ms INTEGER,"
              "  last_anchor TEXT,"
              "  linked_note_path TEXT,"
              "  PRIMARY KEY(workspace_root, paper_id)"
              ");"
              "CREATE TABLE IF NOT EXISTS paper_tags("
              "  workspace_root TEXT NOT NULL,"
              "  paper_id TEXT NOT NULL,"
              "  tag TEXT NOT NULL,"
              "  PRIMARY KEY(workspace_root, paper_id, tag)"
              ");"
              "CREATE TABLE IF NOT EXISTS paper_bookmarks("
              "  workspace_root TEXT NOT NULL,"
              "  paper_id TEXT NOT NULL,"
              "  bookmark_order INTEGER NOT NULL,"
              "  anchor TEXT NOT NULL,"
              "  PRIMARY KEY(workspace_root, paper_id, bookmark_order)"
              ");"
              "COMMIT;")) {
    return false;
  }
  if (!workspace_has_editor_column(db) &&
      !exec(db, "ALTER TABLE workspace_ui_state ADD COLUMN external_editor TEXT NOT NULL DEFAULT 'nvim';")) {
    return false;
  }
  if (!exec(db, "INSERT OR IGNORE INTO schema_migrations(version) VALUES(2);")) {
    return false;
  }
  return true;
}

std::string redact_sensitive_text(std::string text) {
  static const std::regex header(
      R"((authorization\s*:\s*)(bearer\s+)?[^\s]+)", std::regex_constants::icase);
  static const std::regex assignment(
      R"(((api[_-]?key|token|secret|password)\s*[=:]\s*)[^\s,;]+)", std::regex_constants::icase);
  text = std::regex_replace(text, header, "$1[redacted]");
  text = std::regex_replace(text, assignment, "$1[redacted]");
  for (const char* name : {"FINNHUB_API_KEY", "TWELVE_DATA_API_KEY", "API_KEY", "ACCESS_TOKEN"}) {
    if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
      std::string secret(value);
      for (auto at = text.find(secret); at != std::string::npos; at = text.find(secret, at + 10)) {
        text.replace(at, secret.size(), "[redacted]");
      }
    }
  }
  return text;
}

bool task_argv_is_sensitive(const std::vector<std::string>& argv) {
  static const std::regex sensitive(
      R"((authorization|api[_-]?key|token|secret|password))", std::regex_constants::icase);
  return std::any_of(argv.begin(), argv.end(), [](const std::string& arg) {
    return std::regex_search(arg, sensitive);
  });
}

std::optional<WorkspacePersistentState> WorkspaceStore::load(const std::filesystem::path& root) const {
  SqliteDb db(database_file_for(root));
  if (!db || !ensure_schema(db.get())) {
    return std::nullopt;
  }

  WorkspacePersistentState state;
  state.root = root;

  sqlite3_stmt* workspace_stmt = nullptr;
  sqlite3_prepare_v2(db.get(),
                     "SELECT workspace_name, focused_tab, selected_log_source, external_editor, last_anchor "
                     "FROM workspace_ui_state WHERE workspace_root = ?1",
                     -1,
                     &workspace_stmt,
                     nullptr);
  bind_text(workspace_stmt, 1, root.string());
  if (sqlite3_step(workspace_stmt) != SQLITE_ROW) {
    sqlite3_finalize(workspace_stmt);
    return std::nullopt;
  }

  state.name = reinterpret_cast<const char*>(sqlite3_column_text(workspace_stmt, 0));
  state.focused_tab = static_cast<std::size_t>(sqlite3_column_int(workspace_stmt, 1));
  state.selected_log_source = reinterpret_cast<const char*>(sqlite3_column_text(workspace_stmt, 2));
  if (const auto* editor = reinterpret_cast<const char*>(sqlite3_column_text(workspace_stmt, 3))) {
    state.external_editor = editor;
  }
  if (const auto* anchor_text = reinterpret_cast<const char*>(sqlite3_column_text(workspace_stmt, 4))) {
    state.last_anchor = parse_anchor(anchor_text);
  }
  sqlite3_finalize(workspace_stmt);

  sqlite3_stmt* tab_stmt = nullptr;
  sqlite3_prepare_v2(db.get(),
                     "SELECT name, role, focused_pane, layout "
                     "FROM workspace_tabs WHERE workspace_root = ?1 ORDER BY tab_order",
                     -1,
                     &tab_stmt,
                     nullptr);
  bind_text(tab_stmt, 1, root.string());
  while (sqlite3_step(tab_stmt) == SQLITE_ROW) {
    TabPersistentState tab;
    tab.name = reinterpret_cast<const char*>(sqlite3_column_text(tab_stmt, 0));
    auto role = parse_tab_role(reinterpret_cast<const char*>(sqlite3_column_text(tab_stmt, 1)));
    auto pane = parse_pane_kind(reinterpret_cast<const char*>(sqlite3_column_text(tab_stmt, 2)));
    auto layout =
        parse_split_tree(reinterpret_cast<const char*>(sqlite3_column_text(tab_stmt, 3)));
    if (!role || !pane || !layout) {
      sqlite3_finalize(tab_stmt);
      return std::nullopt;
    }
    tab.role = *role;
    tab.focused_pane = *pane;
    tab.layout = std::move(*layout);
    state.tabs.push_back(std::move(tab));
  }
  sqlite3_finalize(tab_stmt);

  sqlite3_stmt* command_stmt = nullptr;
  sqlite3_prepare_v2(db.get(),
                     "SELECT command_text FROM recent_commands "
                     "WHERE workspace_root = ?1 ORDER BY command_order",
                     -1,
                     &command_stmt,
                     nullptr);
  bind_text(command_stmt, 1, root.string());
  while (sqlite3_step(command_stmt) == SQLITE_ROW) {
    state.recent_commands.emplace_back(
        reinterpret_cast<const char*>(sqlite3_column_text(command_stmt, 0)));
  }
  sqlite3_finalize(command_stmt);

  if (state.tabs.empty()) {
    return std::nullopt;
  }
  if (state.focused_tab >= state.tabs.size()) {
    state.focused_tab = 0;
  }
  return state;
}

bool WorkspaceStore::save(const WorkspacePersistentState& state) const {
  std::error_code ec;
  std::filesystem::create_directories(config_dir_for(state.root), ec);
  if (ec) {
    return false;
  }

  SqliteDb db(database_file_for(state.root));
  if (!db || !ensure_schema(db.get()) || !exec(db.get(), "BEGIN;")) {
    return false;
  }

  sqlite3_stmt* workspace_stmt = nullptr;
  sqlite3_prepare_v2(db.get(),
                     "INSERT INTO workspace_ui_state(workspace_root, workspace_name, focused_tab, "
                     "selected_log_source, external_editor, last_anchor) VALUES(?1, ?2, ?3, ?4, ?5, ?6) "
                     "ON CONFLICT(workspace_root) DO UPDATE SET "
                     "workspace_name=excluded.workspace_name, "
                     "focused_tab=excluded.focused_tab, "
                     "selected_log_source=excluded.selected_log_source, "
                     "external_editor=excluded.external_editor, "
                     "last_anchor=excluded.last_anchor",
                     -1,
                     &workspace_stmt,
                     nullptr);
  bind_text(workspace_stmt, 1, state.root.string());
  bind_text(workspace_stmt, 2, state.name);
  sqlite3_bind_int(workspace_stmt, 3, static_cast<int>(state.focused_tab));
  bind_text(workspace_stmt, 4, state.selected_log_source);
  bind_text(workspace_stmt, 5, state.external_editor);
  if (state.last_anchor) {
    bind_text(workspace_stmt, 6, serialize_anchor(*state.last_anchor));
  } else {
    sqlite3_bind_null(workspace_stmt, 6);
  }
  if (sqlite3_step(workspace_stmt) != SQLITE_DONE) {
    sqlite3_finalize(workspace_stmt);
    exec(db.get(), "ROLLBACK;");
    return false;
  }
  sqlite3_finalize(workspace_stmt);

  sqlite3_stmt* delete_tabs = nullptr;
  sqlite3_prepare_v2(
      db.get(), "DELETE FROM workspace_tabs WHERE workspace_root = ?1", -1, &delete_tabs, nullptr);
  bind_text(delete_tabs, 1, state.root.string());
  sqlite3_step(delete_tabs);
  sqlite3_finalize(delete_tabs);

  sqlite3_stmt* insert_tab = nullptr;
  sqlite3_prepare_v2(db.get(),
                     "INSERT INTO workspace_tabs(workspace_root, tab_order, name, role, "
                     "focused_pane, layout) VALUES(?1, ?2, ?3, ?4, ?5, ?6)",
                     -1,
                     &insert_tab,
                     nullptr);
  for (std::size_t i = 0; i < state.tabs.size(); ++i) {
    const auto& tab = state.tabs[i];
    sqlite3_reset(insert_tab);
    sqlite3_clear_bindings(insert_tab);
    bind_text(insert_tab, 1, state.root.string());
    sqlite3_bind_int(insert_tab, 2, static_cast<int>(i));
    bind_text(insert_tab, 3, tab.name);
    bind_text(insert_tab, 4, to_string(tab.role));
    bind_text(insert_tab, 5, to_string(tab.focused_pane));
    bind_text(insert_tab, 6, serialize_split_tree(tab.layout));
    if (sqlite3_step(insert_tab) != SQLITE_DONE) {
      sqlite3_finalize(insert_tab);
      exec(db.get(), "ROLLBACK;");
      return false;
    }
  }
  sqlite3_finalize(insert_tab);

  sqlite3_stmt* delete_commands = nullptr;
  sqlite3_prepare_v2(db.get(),
                     "DELETE FROM recent_commands WHERE workspace_root = ?1",
                     -1,
                     &delete_commands,
                     nullptr);
  bind_text(delete_commands, 1, state.root.string());
  sqlite3_step(delete_commands);
  sqlite3_finalize(delete_commands);

  sqlite3_stmt* insert_command = nullptr;
  sqlite3_prepare_v2(db.get(),
                     "INSERT INTO recent_commands(workspace_root, command_order, command_text) "
                     "VALUES(?1, ?2, ?3)",
                     -1,
                     &insert_command,
                     nullptr);
  for (std::size_t i = 0; i < state.recent_commands.size(); ++i) {
    sqlite3_reset(insert_command);
    sqlite3_clear_bindings(insert_command);
    bind_text(insert_command, 1, state.root.string());
    sqlite3_bind_int(insert_command, 2, static_cast<int>(i));
    bind_text(insert_command, 3, state.recent_commands[i]);
    if (sqlite3_step(insert_command) != SQLITE_DONE) {
      sqlite3_finalize(insert_command);
      exec(db.get(), "ROLLBACK;");
      return false;
    }
  }
  sqlite3_finalize(insert_command);

  return exec(db.get(), "COMMIT;");
}

bool WorkspaceStore::reset_layout(const std::filesystem::path& root) const {
  std::error_code ec;
  std::filesystem::remove(database_file_for(root), ec);
  std::filesystem::remove(state_file_for(root), ec);
  return !ec;
}

std::vector<TaskRecord> WorkspaceStore::load_tasks(const std::filesystem::path& root) const {
  SqliteDb db(database_file_for(root));
  if (!db || !ensure_schema(db.get())) {
    return {};
  }
  std::vector<TaskRecord> tasks;
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db.get(),
                     "SELECT task_order,name,command_text,use_pty,state,exit_code,started_at,finished_at,"
                     "stdout_excerpt,stderr_excerpt,timed_out,cancelled FROM task_history "
                     "WHERE workspace_root=?1 ORDER BY task_order",
                     -1, &stmt, nullptr);
  bind_text(stmt, 1, root.string());
  std::vector<int> orders;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    TaskRecord task;
    orders.push_back(sqlite3_column_int(stmt, 0));
    task.name = column_text(stmt, 1).value_or("");
    task.command = column_text(stmt, 2).value_or("");
    task.use_pty = sqlite3_column_int(stmt, 3) != 0;
    const auto state = column_text(stmt, 4).value_or("failed");
    if (state == "exited") task.state = TaskState::Exited;
    else if (state == "cancelled") task.state = TaskState::Cancelled;
    else task.state = TaskState::Failed;
    task.exit_code = sqlite3_column_int(stmt, 5);
    task.started_at = column_text(stmt, 6).value_or("");
    task.finished_at = column_text(stmt, 7).value_or("");
    task.stdout_excerpt = column_text(stmt, 8).value_or("");
    task.stderr_excerpt = column_text(stmt, 9).value_or("");
    task.timed_out = sqlite3_column_int(stmt, 10) != 0;
    task.cancelled = sqlite3_column_int(stmt, 11) != 0;
    tasks.push_back(std::move(task));
  }
  sqlite3_finalize(stmt);
  sqlite3_stmt* argv_stmt = nullptr;
  sqlite3_prepare_v2(db.get(),
                     "SELECT arg_text FROM task_argv WHERE workspace_root=?1 AND task_order=?2 "
                     "ORDER BY arg_order", -1, &argv_stmt, nullptr);
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    sqlite3_reset(argv_stmt);
    sqlite3_clear_bindings(argv_stmt);
    bind_text(argv_stmt, 1, root.string());
    sqlite3_bind_int(argv_stmt, 2, orders[i]);
    while (sqlite3_step(argv_stmt) == SQLITE_ROW) {
      tasks[i].argv.push_back(column_text(argv_stmt, 0).value_or(""));
    }
  }
  sqlite3_finalize(argv_stmt);
  return tasks;
}

bool WorkspaceStore::save_tasks(const std::filesystem::path& root,
                                const std::vector<TaskRecord>& tasks) const {
  std::error_code ec;
  std::filesystem::create_directories(config_dir_for(root), ec);
  SqliteDb db(database_file_for(root));
  if (ec || !db || !ensure_schema(db.get()) || !exec(db.get(), "BEGIN;")) {
    return false;
  }
  sqlite3_stmt* clear = nullptr;
  sqlite3_prepare_v2(db.get(), "DELETE FROM task_argv WHERE workspace_root=?1", -1, &clear, nullptr);
  bind_text(clear, 1, root.string()); sqlite3_step(clear); sqlite3_finalize(clear);
  sqlite3_prepare_v2(db.get(), "DELETE FROM task_history WHERE workspace_root=?1", -1, &clear, nullptr);
  bind_text(clear, 1, root.string()); sqlite3_step(clear); sqlite3_finalize(clear);

  sqlite3_stmt* task_stmt = nullptr;
  sqlite3_prepare_v2(db.get(),
                     "INSERT INTO task_history VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13)",
                     -1, &task_stmt, nullptr);
  sqlite3_stmt* arg_stmt = nullptr;
  sqlite3_prepare_v2(db.get(), "INSERT INTO task_argv VALUES(?1,?2,?3,?4)", -1, &arg_stmt, nullptr);
  const auto begin = tasks.size() > 50 ? tasks.size() - 50 : 0;
  for (std::size_t source = begin, order = 0; source < tasks.size(); ++source, ++order) {
    auto task = tasks[source];
    const bool sensitive = task_argv_is_sensitive(task.argv);
    if (sensitive) {
      task.command = "[sensitive command omitted]";
    }
    task.stdout_excerpt = redact_sensitive_text(task.stdout_excerpt);
    task.stderr_excerpt = redact_sensitive_text(task.stderr_excerpt);
    if (task.stdout_excerpt.size() > 4096) task.stdout_excerpt.erase(0, task.stdout_excerpt.size() - 4096);
    if (task.stderr_excerpt.size() > 4096) task.stderr_excerpt.erase(0, task.stderr_excerpt.size() - 4096);
    sqlite3_reset(task_stmt); sqlite3_clear_bindings(task_stmt);
    bind_text(task_stmt, 1, root.string()); sqlite3_bind_int(task_stmt, 2, static_cast<int>(order));
    bind_text(task_stmt, 3, task.name); bind_text(task_stmt, 4, task.command);
    sqlite3_bind_int(task_stmt, 5, task.use_pty); bind_text(task_stmt, 6, to_string(task.state));
    sqlite3_bind_int(task_stmt, 7, task.exit_code); bind_text(task_stmt, 8, task.started_at);
    bind_text(task_stmt, 9, task.finished_at); bind_text(task_stmt, 10, task.stdout_excerpt);
    bind_text(task_stmt, 11, task.stderr_excerpt); sqlite3_bind_int(task_stmt, 12, task.timed_out);
    sqlite3_bind_int(task_stmt, 13, task.cancelled);
    if (sqlite3_step(task_stmt) != SQLITE_DONE) {
      sqlite3_finalize(task_stmt); sqlite3_finalize(arg_stmt); exec(db.get(), "ROLLBACK;"); return false;
    }
    if (!sensitive) {
      for (std::size_t arg = 0; arg < task.argv.size(); ++arg) {
        sqlite3_reset(arg_stmt); sqlite3_clear_bindings(arg_stmt);
        bind_text(arg_stmt, 1, root.string()); sqlite3_bind_int(arg_stmt, 2, static_cast<int>(order));
        sqlite3_bind_int(arg_stmt, 3, static_cast<int>(arg)); bind_text(arg_stmt, 4, task.argv[arg]);
        if (sqlite3_step(arg_stmt) != SQLITE_DONE) {
          sqlite3_finalize(task_stmt); sqlite3_finalize(arg_stmt); exec(db.get(), "ROLLBACK;"); return false;
        }
      }
    }
  }
  sqlite3_finalize(task_stmt);
  sqlite3_finalize(arg_stmt);
  return exec(db.get(), "COMMIT;");
}

bool WorkspaceStore::clear_tasks(const std::filesystem::path& root) const {
  return save_tasks(root, {});
}

std::vector<PaperRecord> WorkspaceStore::load_papers(const std::filesystem::path& root) const {
  SqliteDb db(database_file_for(root));
  if (!db || !ensure_schema(db.get())) {
    return {};
  }

  std::vector<PaperRecord> papers;
  sqlite3_stmt* paper_stmt = nullptr;
  sqlite3_prepare_v2(db.get(),
                     "SELECT paper_id, canonical_path, file_size, mtime, title, reading_status, "
                     "last_opened_unix_ms, last_anchor, linked_note_path "
                     "FROM papers WHERE workspace_root = ?1 ORDER BY title, paper_id",
                     -1,
                     &paper_stmt,
                     nullptr);
  bind_text(paper_stmt, 1, root.string());
  while (sqlite3_step(paper_stmt) == SQLITE_ROW) {
    PaperRecord paper;
    paper.paper_id = column_text(paper_stmt, 0).value_or("");
    paper.canonical_path = column_text(paper_stmt, 1).value_or("");
    paper.file_size = static_cast<std::uintmax_t>(sqlite3_column_int64(paper_stmt, 2));
    paper.mtime = sqlite3_column_int64(paper_stmt, 3);
    paper.title = column_text(paper_stmt, 4).value_or("");
    paper.reading_status = column_text(paper_stmt, 5).value_or("unread");
    if (sqlite3_column_type(paper_stmt, 6) != SQLITE_NULL) {
      paper.last_opened_unix_ms = sqlite3_column_int64(paper_stmt, 6);
    }
    if (auto anchor = column_text(paper_stmt, 7)) {
      paper.last_anchor = parse_anchor(*anchor);
    }
    if (auto note = column_text(paper_stmt, 8)) {
      paper.linked_note_path = std::filesystem::path(*note);
    }
    papers.push_back(std::move(paper));
  }
  sqlite3_finalize(paper_stmt);

  for (auto& paper : papers) {
    sqlite3_stmt* tag_stmt = nullptr;
    sqlite3_prepare_v2(db.get(),
                       "SELECT tag FROM paper_tags WHERE workspace_root = ?1 AND paper_id = ?2 ORDER BY tag",
                       -1,
                       &tag_stmt,
                       nullptr);
    bind_text(tag_stmt, 1, root.string());
    bind_text(tag_stmt, 2, paper.paper_id);
    while (sqlite3_step(tag_stmt) == SQLITE_ROW) {
      paper.tags.push_back(column_text(tag_stmt, 0).value_or(""));
    }
    sqlite3_finalize(tag_stmt);

    sqlite3_stmt* bookmark_stmt = nullptr;
    sqlite3_prepare_v2(db.get(),
                       "SELECT anchor FROM paper_bookmarks "
                       "WHERE workspace_root = ?1 AND paper_id = ?2 ORDER BY bookmark_order",
                       -1,
                       &bookmark_stmt,
                       nullptr);
    bind_text(bookmark_stmt, 1, root.string());
    bind_text(bookmark_stmt, 2, paper.paper_id);
    while (sqlite3_step(bookmark_stmt) == SQLITE_ROW) {
      if (auto anchor = parse_anchor(column_text(bookmark_stmt, 0).value_or(""))) {
        paper.bookmarks.push_back(*anchor);
      }
    }
    sqlite3_finalize(bookmark_stmt);
  }

  return papers;
}

bool WorkspaceStore::save_paper(const std::filesystem::path& root, const PaperRecord& paper) const {
  std::error_code ec;
  std::filesystem::create_directories(config_dir_for(root), ec);
  if (ec) {
    return false;
  }

  SqliteDb db(database_file_for(root));
  if (!db || !ensure_schema(db.get()) || !exec(db.get(), "BEGIN;")) {
    return false;
  }

  sqlite3_stmt* paper_stmt = nullptr;
  sqlite3_prepare_v2(db.get(),
                     "INSERT INTO papers(workspace_root, paper_id, canonical_path, file_size, mtime, title, "
                     "reading_status, last_opened_unix_ms, last_anchor, linked_note_path) "
                     "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10) "
                     "ON CONFLICT(workspace_root, paper_id) DO UPDATE SET "
                     "canonical_path=excluded.canonical_path, "
                     "file_size=excluded.file_size, "
                     "mtime=excluded.mtime, "
                     "title=excluded.title, "
                     "reading_status=excluded.reading_status, "
                     "last_opened_unix_ms=excluded.last_opened_unix_ms, "
                     "last_anchor=excluded.last_anchor, "
                     "linked_note_path=excluded.linked_note_path",
                     -1,
                     &paper_stmt,
                     nullptr);
  bind_text(paper_stmt, 1, root.string());
  bind_text(paper_stmt, 2, paper.paper_id);
  bind_text(paper_stmt, 3, paper.canonical_path.string());
  sqlite3_bind_int64(paper_stmt, 4, static_cast<sqlite3_int64>(paper.file_size));
  sqlite3_bind_int64(paper_stmt, 5, static_cast<sqlite3_int64>(paper.mtime));
  bind_text(paper_stmt, 6, paper.title);
  bind_text(paper_stmt, 7, paper.reading_status);
  if (paper.last_opened_unix_ms) {
    sqlite3_bind_int64(paper_stmt, 8, *paper.last_opened_unix_ms);
  } else {
    sqlite3_bind_null(paper_stmt, 8);
  }
  if (paper.last_anchor) {
    bind_text(paper_stmt, 9, serialize_anchor(*paper.last_anchor));
  } else {
    sqlite3_bind_null(paper_stmt, 9);
  }
  if (paper.linked_note_path) {
    bind_text(paper_stmt, 10, paper.linked_note_path->string());
  } else {
    sqlite3_bind_null(paper_stmt, 10);
  }
  if (sqlite3_step(paper_stmt) != SQLITE_DONE) {
    sqlite3_finalize(paper_stmt);
    exec(db.get(), "ROLLBACK;");
    return false;
  }
  sqlite3_finalize(paper_stmt);

  sqlite3_stmt* delete_tags = nullptr;
  sqlite3_prepare_v2(db.get(),
                     "DELETE FROM paper_tags WHERE workspace_root = ?1 AND paper_id = ?2",
                     -1,
                     &delete_tags,
                     nullptr);
  bind_text(delete_tags, 1, root.string());
  bind_text(delete_tags, 2, paper.paper_id);
  sqlite3_step(delete_tags);
  sqlite3_finalize(delete_tags);

  sqlite3_stmt* insert_tag = nullptr;
  sqlite3_prepare_v2(db.get(),
                     "INSERT INTO paper_tags(workspace_root, paper_id, tag) VALUES(?1, ?2, ?3)",
                     -1,
                     &insert_tag,
                     nullptr);
  for (const auto& tag : paper.tags) {
    sqlite3_reset(insert_tag);
    sqlite3_clear_bindings(insert_tag);
    bind_text(insert_tag, 1, root.string());
    bind_text(insert_tag, 2, paper.paper_id);
    bind_text(insert_tag, 3, tag);
    if (sqlite3_step(insert_tag) != SQLITE_DONE) {
      sqlite3_finalize(insert_tag);
      exec(db.get(), "ROLLBACK;");
      return false;
    }
  }
  sqlite3_finalize(insert_tag);

  sqlite3_stmt* delete_bookmarks = nullptr;
  sqlite3_prepare_v2(db.get(),
                     "DELETE FROM paper_bookmarks WHERE workspace_root = ?1 AND paper_id = ?2",
                     -1,
                     &delete_bookmarks,
                     nullptr);
  bind_text(delete_bookmarks, 1, root.string());
  bind_text(delete_bookmarks, 2, paper.paper_id);
  sqlite3_step(delete_bookmarks);
  sqlite3_finalize(delete_bookmarks);

  sqlite3_stmt* insert_bookmark = nullptr;
  sqlite3_prepare_v2(db.get(),
                     "INSERT INTO paper_bookmarks(workspace_root, paper_id, bookmark_order, anchor) "
                     "VALUES(?1, ?2, ?3, ?4)",
                     -1,
                     &insert_bookmark,
                     nullptr);
  for (std::size_t i = 0; i < paper.bookmarks.size(); ++i) {
    sqlite3_reset(insert_bookmark);
    sqlite3_clear_bindings(insert_bookmark);
    bind_text(insert_bookmark, 1, root.string());
    bind_text(insert_bookmark, 2, paper.paper_id);
    sqlite3_bind_int(insert_bookmark, 3, static_cast<int>(i));
    bind_text(insert_bookmark, 4, serialize_anchor(paper.bookmarks[i]));
    if (sqlite3_step(insert_bookmark) != SQLITE_DONE) {
      sqlite3_finalize(insert_bookmark);
      exec(db.get(), "ROLLBACK;");
      return false;
    }
  }
  sqlite3_finalize(insert_bookmark);

  return exec(db.get(), "COMMIT;");
}

}  // namespace deck
