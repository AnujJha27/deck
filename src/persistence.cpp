#include "deck/persistence.h"
#include "deck/split_tree.h"

#include <sqlite3.h>

#include <filesystem>
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
