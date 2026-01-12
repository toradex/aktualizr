#include "offline_logs_db.h"

#include <boost/filesystem.hpp>

#include "logging/logging.h"

namespace {

// Schema version for future migrations
constexpr int kSchemaVersion = 1;

// SQL schema for the offline logs database
const char* const kSchema = R"(
CREATE TABLE IF NOT EXISTS version (
    version INTEGER PRIMARY KEY
);

INSERT OR IGNORE INTO version VALUES (1);

CREATE TABLE IF NOT EXISTS installs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id TEXT NOT NULL,
    name TEXT,
    version INTEGER,
    report_counter INTEGER,
    manifest TEXT
);

CREATE TABLE IF NOT EXISTS logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    install_id INTEGER NOT NULL,
    timestamp INTEGER NOT NULL,
    service TEXT NOT NULL,
    message TEXT NOT NULL,
    FOREIGN KEY (install_id) REFERENCES installs(id)
);

CREATE TABLE IF NOT EXISTS reports (
    install_id INTEGER NOT NULL,
    report_id TEXT NOT NULL,
    timestamp INTEGER NOT NULL,
    type TEXT NOT NULL,
    version INTEGER NOT NULL,
    event TEXT NOT NULL,
    PRIMARY KEY (install_id, report_id),
    FOREIGN KEY (install_id) REFERENCES installs(id)
);

CREATE INDEX IF NOT EXISTS idx_installs_device_id ON installs(device_id);
CREATE INDEX IF NOT EXISTS idx_logs_install_id ON logs(install_id);
CREATE INDEX IF NOT EXISTS idx_reports_install_id ON reports(install_id);
)";

}  // namespace

OfflineLogsDb::OfflineLogsDb(const boost::filesystem::path& db_path) : db_path_(db_path) {
  // Check if parent directory exists and is writable
  boost::filesystem::path parent_dir = db_path.parent_path();
  if (!parent_dir.empty() && !boost::filesystem::exists(parent_dir)) {
    LOG_WARNING << "Offline logs db parent directory does not exist: " << parent_dir;
    return;
  }

  // Security: Use SQLITE_OPEN_NOFOLLOW to refuse to follow symlinks.
  // This prevents symlink attacks where a user creates a symlink pointing
  // to a sensitive system file. The check is atomic (no TOCTOU vulnerability).
  constexpr bool kReadonly = false;
  constexpr bool kNofollow = true;
  db_.emplace(db_path, kReadonly, nullptr, kNofollow);
  if (db_->get_rc() != SQLITE_OK) {
    LOG_WARNING << "Can't open offline logs database: " << db_->errmsg();
    db_.reset();
    return;
  }

  // Try to initialize the schema
  if (!InitializeSchema()) {
    LOG_WARNING << "Failed to initialize offline logs database at " << db_path;
    db_.reset();
    return;
  }
}

bool OfflineLogsDb::InitializeSchema() {
  // assert this invariant in order to silence bugprone-unchecked-optional-access
  // clang-tidy warnings
  if (!db_.has_value()) {
    assert(0);
    return false;
  }

  try {
    // Check if we already have the schema by checking for the version table
    auto statement = db_->prepareStatement("SELECT count(*) FROM sqlite_master WHERE type='table' AND name='version';");
    if (statement.step() != SQLITE_ROW) {
      LOG_ERROR << "Can't check for existing schema: " << db_->errmsg();
      return false;
    }

    int64_t table_count = statement.get_result_col_int(0);

    if (table_count == 0) {
      // No existing schema, create it
      LOG_INFO << "Creating offline logs database schema at " << db_path_;
      if (db_->exec(kSchema, nullptr, nullptr) != SQLITE_OK) {
        LOG_ERROR << "Can't create offline logs schema: " << db_->errmsg();
        return false;
      }
    } else {
      // Verify schema version
      auto version_stmt = db_->prepareStatement("SELECT version FROM version LIMIT 1;");
      if (version_stmt.step() == SQLITE_ROW) {
        int64_t version = version_stmt.get_result_col_int(0);
        if (version > kSchemaVersion) {
          LOG_WARNING << "Offline logs database has newer schema version " << version << " (expected " << kSchemaVersion
                      << ")";
          // Continue anyway - we might be able to read/write compatible data
        }
      }
    }

    return true;
  } catch (const SQLException& e) {
    LOG_WARNING << "SQL exception initializing offline logs database: " << e.what();
    return false;
  } catch (const std::exception& e) {
    LOG_WARNING << "Exception initializing offline logs database: " << e.what();
    return false;
  }
}

InstallId OfflineLogsDb::CreateInstall(std::string_view device_id, std::string_view name, int version) {
  if (!db_.has_value()) {
    LOG_WARNING << "Attempt to create install on failed database";
    return InstallId();
  }

  try {
    auto statement = db_->prepareStatement("INSERT INTO installs (device_id, name, version) VALUES (?, ?, ?);",
                                           std::string(device_id), std::string(name), version);

    if (statement.step() != SQLITE_DONE) {
      LOG_ERROR << "Can't create install record: " << db_->errmsg();
      db_.reset();
      return InstallId();
    }

    int64_t row_id = sqlite3_last_insert_rowid(db_->get());
    LOG_DEBUG << "Created install record with id " << row_id << " for device " << device_id;
    return InstallId::FromDb(row_id);

  } catch (const SQLException& e) {
    LOG_ERROR << "SQL exception creating install: " << e.what();
    db_.reset();
    return InstallId();
  }
}

InstallId OfflineLogsDb::FindInProgressInstall(std::string_view device_id) {
  if (!db_.has_value()) {
    LOG_WARNING << "Attempt to find install on failed database";
    return InstallId();
  }

  try {
    // Find the most recent install for this device with null manifest (in-progress)
    auto statement = db_->prepareStatement(
        "SELECT id FROM installs WHERE device_id = ? AND manifest IS NULL ORDER BY id DESC LIMIT 1;",
        std::string(device_id));

    if (statement.step() != SQLITE_ROW) {
      // No in-progress install found - this is normal, not an error
      return InstallId();
    }

    int64_t row_id = statement.get_result_col_int(0);
    LOG_DEBUG << "Found in-progress install with id " << row_id << " for device " << device_id;
    return InstallId::FromDb(row_id);

  } catch (const SQLException& e) {
    LOG_ERROR << "SQL exception finding in-progress install: " << e.what();
    db_.reset();
    return InstallId();
  }
}

void OfflineLogsDb::CompleteInstall(InstallId install_id, int64_t report_counter, std::string_view manifest) {
  if (!install_id.IsValid()) {
    LOG_WARNING << "Attempt to complete invalid install_id";
    return;
  }

  if (!db_.has_value()) {
    LOG_WARNING << "Attempt to complete install on failed database";
    return;
  }

  try {
    auto statement = db_->prepareStatement("UPDATE installs SET report_counter = ?, manifest = ? WHERE id = ?;",
                                           report_counter, std::string(manifest), install_id.Value());

    if (statement.step() != SQLITE_DONE) {
      LOG_ERROR << "Can't complete install record: " << db_->errmsg();
      db_.reset();
      return;
    }

    int changes = sqlite3_changes(db_->get());
    if (changes == 0) {
      LOG_WARNING << "No install record found with id " << install_id.Value();
    } else {
      LOG_DEBUG << "Completed install record with id " << install_id.Value();
    }

  } catch (const SQLException& e) {
    LOG_ERROR << "SQL exception completing install: " << e.what();
    db_.reset();
  }
}

void OfflineLogsDb::AddLogEntry(InstallId install_id, int64_t timestamp_us, std::string_view service,
                                std::string_view message) {
  if (!install_id.IsValid()) {
    LOG_WARNING << "Attempt to add log entry with invalid install_id";
    return;
  }

  if (!db_.has_value()) {
    LOG_WARNING << "Attempt to add log entry on failed database";
    return;
  }

  try {
    auto statement =
        db_->prepareStatement("INSERT INTO logs (install_id, timestamp, service, message) VALUES (?, ?, ?, ?);",
                              install_id.Value(), timestamp_us, std::string(service), std::string(message));

    if (statement.step() != SQLITE_DONE) {
      LOG_ERROR << "Can't add log entry: " << db_->errmsg();
      db_.reset();
    }

  } catch (const SQLException& e) {
    LOG_ERROR << "SQL exception adding log entry: " << e.what();
    db_.reset();
  }
}

void OfflineLogsDb::AddReport(InstallId install_id, std::string_view report_id, int64_t timestamp_us,
                              std::string_view type, int version, std::string_view event_json) {
  if (!install_id.IsValid()) {
    LOG_WARNING << "Attempt to add report with invalid install_id";
    return;
  }

  if (!db_.has_value()) {
    LOG_WARNING << "Attempt to add report on failed database";
    return;
  }

  try {
    // Use INSERT OR IGNORE to handle duplicates gracefully (report_id is part of primary key)
    auto statement = db_->prepareStatement(
        "INSERT OR IGNORE INTO reports (install_id, report_id, timestamp, type, version, event) "
        "VALUES (?, ?, ?, ?, ?, ?);",
        install_id.Value(), std::string(report_id), timestamp_us, std::string(type), version, std::string(event_json));

    if (statement.step() != SQLITE_DONE) {
      LOG_ERROR << "Can't add report: " << db_->errmsg();
      db_.reset();
    }

  } catch (const SQLException& e) {
    LOG_ERROR << "SQL exception adding report: " << e.what();
    db_.reset();
  }
}
