#define _POSIX_C_SOURCE 200809L
#include "db.h"

#include "constants.h"
#include "filter.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct db_context {
  sqlite3 *connection;
  sqlite3_stmt *statement_get_file_metadata;
  sqlite3_stmt *statement_upsert_file;
  sqlite3_stmt *statement_update_file_modification_time;
  sqlite3_stmt *statement_record_run;
};

static const char SQL_GET_FILE_METADATA[] = "SELECT size, mtime, sha256 FROM files WHERE path = ?1 LIMIT 1;";

/**
 * Prepares one reusable statement and reports the SQLite error on failure.
 */
static bool prepare_statement(sqlite3 *connection, const char *sql, sqlite3_stmt **output_statement) {
  if (sqlite3_prepare_v2(connection, sql, -1, output_statement, NULL) == SQLITE_OK) return true;
  fprintf(stderr, "Failed to prepare SQLite statement: %s\n", sqlite3_errmsg(connection));
  return false;
}

/**
 * Executes a transaction statement and reports the SQLite error on failure.
 */
static bool execute_transaction_statement(db_t *database, const char *sql, const char *operation) {
  char *error_message = NULL;
  int result_code = sqlite3_exec(database->connection, sql, NULL, NULL, &error_message);
  if (result_code == SQLITE_OK) return true;

  fprintf(stderr, "SQLite %s failed: %s\n", operation, error_message ? error_message : "unknown error");
  sqlite3_free(error_message);
  return false;
}

/**
 * Safely duplicates a string, returning NULL if input is NULL.
 */
static inline char *safe_strdup(const char *source) {
  return source ? strdup(source) : NULL;
}

/**
 * Custom SQLite function that extracts a file extension from a path.
 */
static void sqlite_extension_function(sqlite3_context *context, int argument_count, sqlite3_value **arguments) {
  if (argument_count != 1 || sqlite3_value_type(arguments[0]) == SQLITE_NULL) {
    sqlite3_result_text(context, "none", -1, SQLITE_STATIC);
    return;
  }
  const unsigned char *path = sqlite3_value_text(arguments[0]);
  if (!path) {
    sqlite3_result_text(context, "none", -1, SQLITE_STATIC);
    return;
  }
  const char *extension = filter_get_extension((const char *)path);
  if (extension && extension[0] != '\0') sqlite3_result_text(context, extension, -1, SQLITE_TRANSIENT);
  else sqlite3_result_text(context, "none", -1, SQLITE_STATIC);
}

/**
 * Creates the necessary SQLite schema and indexes if they do not exist.
 */
static bool create_tables(sqlite3 *database_connection) {
  const char *schema = "PRAGMA journal_mode = WAL;"
                       "PRAGMA synchronous = NORMAL;"
                       "CREATE TABLE IF NOT EXISTS files ("
                       "  path TEXT PRIMARY KEY,"
                       "  size INTEGER NOT NULL,"
                       "  mtime INTEGER NOT NULL,"
                       "  sha256 TEXT NOT NULL,"
                       "  physical_bytes INTEGER NOT NULL,"
                       "  compressed_bytes INTEGER NOT NULL,"
                       "  last_compressed_at INTEGER NOT NULL,"
                       "  compressor TEXT NOT NULL,"
                       "  status TEXT NOT NULL"
                       ");"
                       "CREATE TABLE IF NOT EXISTS runs ("
                       "  run_id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "  started_at INTEGER NOT NULL,"
                       "  completed_at INTEGER NOT NULL,"
                       "  files_scanned INTEGER NOT NULL,"
                       "  files_compressed INTEGER NOT NULL,"
                       "  bytes_saved INTEGER NOT NULL,"
                       "  duration_ms INTEGER NOT NULL"
                       ");"
                       "CREATE INDEX IF NOT EXISTS idx_files_status ON files(status);"
                       "CREATE INDEX IF NOT EXISTS idx_files_mtime ON files(mtime);";

  char *error_message = NULL;
  int result_code = sqlite3_exec(database_connection, schema, NULL, NULL, &error_message);
  if (result_code != SQLITE_OK) {
    fprintf(stderr, "SQLite schema error: %s\n", error_message ? error_message : "unknown error");
    sqlite3_free(error_message);
    return false;
  }
  return true;
}

/**
 * Creates parent directories recursively for the specified path.
 */
static void ensure_parent_directory(const char *path) {
  if (!path) return;
  char *path_copy = safe_strdup(path);
  if (!path_copy) return;

  for (char *path_cursor = path_copy + 1; *path_cursor; path_cursor++) {
    if (*path_cursor == '/') {
      *path_cursor = '\0';
      mkdir(path_copy, DIRECTORY_CREATION_PERMISSIONS);
      *path_cursor = '/';
    }
  }
  free(path_copy);
}

/**
 * Opens or initializes a SQLite compression state database.
 */
db_t *db_open(const char *database_path) {
  if (!database_path) return NULL;
  ensure_parent_directory(database_path);

  db_t *database_context = calloc(1, sizeof(db_t));
  if (!database_context) return NULL;

  int result_code = sqlite3_open(database_path, &database_context->connection);
  if (result_code != SQLITE_OK) {
    fprintf(stderr, "Failed to open SQLite database %s: %s\n", database_path,
            sqlite3_errmsg(database_context->connection));
    sqlite3_close(database_context->connection);
    free(database_context);
    return NULL;
  }

  if (!create_tables(database_context->connection)) {
    sqlite3_close(database_context->connection);
    free(database_context);
    return NULL;
  }

  if (sqlite3_create_function(database_context->connection, "get_extension", 1, SQLITE_UTF8, NULL,
                              sqlite_extension_function, NULL, NULL)
      != SQLITE_OK) {
    db_close(database_context);
    return NULL;
  }

  if (!prepare_statement(database_context->connection, SQL_GET_FILE_METADATA,
                         &database_context->statement_get_file_metadata)) {
    db_close(database_context);
    return NULL;
  }

  const char *sql_upsert = "INSERT INTO files ("
                           "  path, size, mtime, sha256, physical_bytes, compressed_bytes, "
                           "  last_compressed_at, compressor, status"
                           ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9) "
                           "ON CONFLICT(path) DO UPDATE SET "
                           "  size = excluded.size,"
                           "  mtime = excluded.mtime,"
                           "  sha256 = excluded.sha256,"
                           "  physical_bytes = excluded.physical_bytes,"
                           "  compressed_bytes = excluded.compressed_bytes,"
                           "  last_compressed_at = excluded.last_compressed_at,"
                           "  compressor = excluded.compressor,"
                           "  status = excluded.status;";
  if (!prepare_statement(database_context->connection, sql_upsert, &database_context->statement_upsert_file)) {
    db_close(database_context);
    return NULL;
  }

  const char *sql_update_modification_time = "UPDATE files SET mtime = ?2 WHERE path = ?1;";
  if (!prepare_statement(database_context->connection, sql_update_modification_time,
                         &database_context->statement_update_file_modification_time)) {
    db_close(database_context);
    return NULL;
  }

  const char *sql_run = "INSERT INTO runs ("
                        "  started_at, completed_at, files_scanned, files_compressed, "
                        "  bytes_saved, duration_ms"
                        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6);";
  if (!prepare_statement(database_context->connection, sql_run, &database_context->statement_record_run)) {
    db_close(database_context);
    return NULL;
  }

  return database_context;
}

/**
 * Opens a concurrent read-only connection to the same SQLite database.
 */
db_t *db_open_read_only(const db_t *database) {
  if (!database || !database->connection) return NULL;

  const char *database_path = sqlite3_db_filename(database->connection, "main");
  if (!database_path || database_path[0] == '\0') return NULL;

  db_t *database_context = calloc(1, sizeof(db_t));
  if (!database_context) return NULL;

  int open_flags = SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX;
  if (sqlite3_open_v2(database_path, &database_context->connection, open_flags, NULL) != SQLITE_OK) {
    fprintf(stderr, "Failed to open SQLite reader for %s: %s\n", database_path,
            sqlite3_errmsg(database_context->connection));
    sqlite3_close(database_context->connection);
    free(database_context);
    return NULL;
  }

  if (!prepare_statement(database_context->connection, SQL_GET_FILE_METADATA,
                         &database_context->statement_get_file_metadata)) {
    db_close(database_context);
    return NULL;
  }
  return database_context;
}

/**
 * Finalizes prepared statements and closes the SQLite database connection.
 */
void db_close(db_t *database) {
  if (!database) return;
  if (database->statement_get_file_metadata) sqlite3_finalize(database->statement_get_file_metadata);
  if (database->statement_upsert_file) sqlite3_finalize(database->statement_upsert_file);
  if (database->statement_update_file_modification_time)
    sqlite3_finalize(database->statement_update_file_modification_time);
  if (database->statement_record_run) sqlite3_finalize(database->statement_record_run);
  if (database->connection) sqlite3_close(database->connection);
  free(database);
}

/**
 * Begins an immediate SQLite transaction.
 */
bool db_begin(db_t *database) {
  if (!database || !database->connection) return false;
  return execute_transaction_statement(database, "BEGIN TRANSACTION;", "begin transaction");
}

/**
 * Commits the current SQLite transaction.
 */
bool db_commit(db_t *database) {
  if (!database || !database->connection) return false;
  return execute_transaction_statement(database, "COMMIT;", "commit");
}

/**
 * Rolls back the current SQLite transaction.
 */
bool db_rollback(db_t *database) {
  if (!database || !database->connection) return false;
  return execute_transaction_statement(database, "ROLLBACK;", "rollback");
}

/**
 * Retrieves only the fields used by the scanner's common unchanged-file path.
 */
db_lookup_result_t db_get_file_metadata(db_t *database, const char *path, file_metadata_t *output_metadata) {
  if (!database || !database->statement_get_file_metadata || !path || !output_metadata) return DB_LOOKUP_ERROR;
  memset(output_metadata, 0, sizeof(*output_metadata));

  sqlite3_stmt *statement = database->statement_get_file_metadata;
  sqlite3_reset(statement);
  sqlite3_clear_bindings(statement);
  sqlite3_bind_text(statement, 1, path, -1, SQLITE_STATIC);

  int result_code = sqlite3_step(statement);
  if (result_code == SQLITE_DONE) return DB_LOOKUP_NOT_FOUND;
  if (result_code != SQLITE_ROW) {
    fprintf(stderr, "SQLite file lookup failed: %s\n", sqlite3_errmsg(database->connection));
    return DB_LOOKUP_ERROR;
  }

  output_metadata->size = sqlite3_column_int64(statement, 0);
  output_metadata->modification_time = sqlite3_column_int64(statement, 1);
  const char *hash = (const char *)sqlite3_column_text(statement, 2);
  if (hash) {
    strncpy(output_metadata->sha256, hash, sizeof(output_metadata->sha256) - 1);
    output_metadata->sha256[sizeof(output_metadata->sha256) - 1] = '\0';
  }
  return DB_LOOKUP_FOUND;
}

/**
 * Inserts or updates a file record in the database.
 */
bool db_upsert_file(db_t *database, const file_record_t *record) {
  if (!database || !database->statement_upsert_file || !record || !record->path) return false;

  sqlite3_stmt *statement = database->statement_upsert_file;
  sqlite3_reset(statement);
  sqlite3_clear_bindings(statement);

  sqlite3_bind_text(statement, 1, record->path, -1, SQLITE_STATIC);
  sqlite3_bind_int64(statement, 2, record->size);
  sqlite3_bind_int64(statement, 3, record->modification_time);
  sqlite3_bind_text(statement, 4, record->sha256, -1, SQLITE_STATIC);
  sqlite3_bind_int64(statement, 5, record->physical_bytes);
  sqlite3_bind_int64(statement, 6, record->compressed_bytes);
  sqlite3_bind_int64(statement, 7, record->last_compressed_at);
  sqlite3_bind_text(statement, 8, record->compressor ? record->compressor : "LZFSE", -1, SQLITE_STATIC);
  sqlite3_bind_text(statement, 9, record->status ? record->status : "compressed", -1, SQLITE_STATIC);

  int result_code = sqlite3_step(statement);
  if (result_code != SQLITE_DONE)
    fprintf(stderr, "SQLite file upsert failed: %s\n", sqlite3_errmsg(database->connection));
  return result_code == SQLITE_DONE;
}

/**
 * Updates the modification time after a successful hash verification.
 */
bool db_update_file_modification_time(db_t *database, const char *path, int64_t modification_time) {
  if (!database || !database->statement_update_file_modification_time || !path) return false;

  sqlite3_stmt *statement = database->statement_update_file_modification_time;
  sqlite3_reset(statement);
  sqlite3_clear_bindings(statement);
  sqlite3_bind_text(statement, 1, path, -1, SQLITE_STATIC);
  sqlite3_bind_int64(statement, 2, modification_time);
  int result_code = sqlite3_step(statement);
  if (result_code != SQLITE_DONE)
    fprintf(stderr, "SQLite modification-time update failed: %s\n", sqlite3_errmsg(database->connection));
  return result_code == SQLITE_DONE;
}

/**
 * Inserts a completed scan run summary record.
 */
bool db_record_run(db_t *database, const run_record_t *run) {
  if (!database || !database->statement_record_run || !run) return false;

  sqlite3_stmt *statement = database->statement_record_run;
  sqlite3_reset(statement);
  sqlite3_clear_bindings(statement);

  sqlite3_bind_int64(statement, 1, run->started_at);
  sqlite3_bind_int64(statement, 2, run->completed_at);
  sqlite3_bind_int64(statement, 3, run->files_scanned);
  sqlite3_bind_int64(statement, 4, run->files_compressed);
  sqlite3_bind_int64(statement, 5, run->bytes_saved);
  sqlite3_bind_int64(statement, 6, run->duration_milliseconds);

  int result_code = sqlite3_step(statement);
  if (result_code != SQLITE_DONE)
    fprintf(stderr, "SQLite run-record insert failed: %s\n", sqlite3_errmsg(database->connection));
  return result_code == SQLITE_DONE;
}

/**
 * Queries aggregate compression and savings statistics.
 */
bool db_get_summary(db_t *database, database_summary_t *output_summary) {
  if (!database || !database->connection || !output_summary) return false;
  memset(output_summary, 0, sizeof(*output_summary));

  const char *sql = "SELECT "
                    "  COUNT(*), "
                    "  COALESCE(SUM(CASE WHEN status = 'compressed' THEN 1 ELSE 0 END), 0), "
                    "  COALESCE(SUM(CASE WHEN status = 'incompressible' THEN 1 ELSE 0 END), 0), "
                    "  COALESCE(SUM(size), 0), "
                    "  COALESCE(SUM(physical_bytes), 0), "
                    "  COALESCE(SUM(CASE WHEN size > physical_bytes THEN size - physical_bytes ELSE 0 END), "
                    "0) "
                    "FROM files;";

  sqlite3_stmt *statement = NULL;
  if (sqlite3_prepare_v2(database->connection, sql, -1, &statement, NULL) != SQLITE_OK) return false;

  if (sqlite3_step(statement) == SQLITE_ROW) {
    output_summary->total_files = sqlite3_column_int64(statement, 0);
    output_summary->total_compressed = sqlite3_column_int64(statement, 1);
    output_summary->total_incompressible = sqlite3_column_int64(statement, 2);
    output_summary->total_logical_bytes = sqlite3_column_int64(statement, 3);
    output_summary->total_physical_bytes = sqlite3_column_int64(statement, 4);
    output_summary->total_bytes_saved = sqlite3_column_int64(statement, 5);
    sqlite3_finalize(statement);
    return true;
  }
  sqlite3_finalize(statement);
  return false;
}

/**
 * Retrieves the most recent scan run records up to the specified limit.
 */
bool db_get_recent_runs(db_t *database, size_t limit, run_record_t **output_runs, size_t *output_count) {
  if (!database || !database->connection || !output_runs || !output_count) return false;
  *output_runs = NULL;
  *output_count = 0;

  const char *sql = "SELECT run_id, started_at, completed_at, files_scanned, files_compressed, "
                    "       bytes_saved, duration_ms "
                    "FROM runs ORDER BY run_id DESC LIMIT ?1;";

  sqlite3_stmt *statement = NULL;
  if (sqlite3_prepare_v2(database->connection, sql, -1, &statement, NULL) != SQLITE_OK) return false;
  sqlite3_bind_int64(statement, 1, (sqlite3_int64)limit);

  run_record_t *runs = malloc(limit * sizeof(run_record_t));
  if (!runs) {
    sqlite3_finalize(statement);
    return false;
  }

  size_t count = 0;
  while (count < limit && sqlite3_step(statement) == SQLITE_ROW) {
    run_record_t *run = &runs[count++];
    run->run_id = sqlite3_column_int64(statement, 0);
    run->started_at = sqlite3_column_int64(statement, 1);
    run->completed_at = sqlite3_column_int64(statement, 2);
    run->files_scanned = sqlite3_column_int64(statement, 3);
    run->files_compressed = sqlite3_column_int64(statement, 4);
    run->bytes_saved = sqlite3_column_int64(statement, 5);
    run->duration_milliseconds = sqlite3_column_int64(statement, 6);
  }

  sqlite3_finalize(statement);
  *output_runs = runs;
  *output_count = count;
  return true;
}

/**
 * Frees memory allocated for an array of run records.
 */
void db_free_recent_runs(run_record_t *runs, size_t count) {
  (void)count;
  free(runs);
}

/**
 * Aggregates file count and savings grouped by file extension.
 */
bool db_get_extension_stats(db_t *database, extension_statistic_t **output_statistics, size_t *output_count) {
  if (!database || !database->connection || !output_statistics || !output_count) return false;
  *output_statistics = NULL;
  *output_count = 0;

  const char *sql = "SELECT "
                    "  get_extension(path) AS ext, "
                    "  COUNT(*), "
                    "  SUM(size), "
                    "  SUM(physical_bytes), "
                    "  SUM(CASE WHEN size > physical_bytes THEN size - physical_bytes ELSE 0 END) AS saved "
                    "FROM files "
                    "GROUP BY ext "
                    "HAVING saved > 0 "
                    "ORDER BY saved DESC LIMIT ?1;";

  sqlite3_stmt *statement = NULL;
  if (sqlite3_prepare_v2(database->connection, sql, -1, &statement, NULL) != SQLITE_OK) return false;
  sqlite3_bind_int64(statement, 1, DEFAULT_EXTENSION_STATS_LIMIT);

  extension_statistic_t *statistics = malloc(DEFAULT_EXTENSION_STATS_LIMIT * sizeof(extension_statistic_t));
  if (!statistics) {
    sqlite3_finalize(statement);
    return false;
  }

  size_t count = 0;
  while (count < DEFAULT_EXTENSION_STATS_LIMIT && sqlite3_step(statement) == SQLITE_ROW) {
    extension_statistic_t *statistic = &statistics[count++];
    statistic->extension = safe_strdup((const char *)sqlite3_column_text(statement, 0));
    statistic->file_count = sqlite3_column_int64(statement, 1);
    statistic->logical_bytes = sqlite3_column_int64(statement, 2);
    statistic->physical_bytes = sqlite3_column_int64(statement, 3);
    statistic->saved_bytes = sqlite3_column_int64(statement, 4);
  }

  sqlite3_finalize(statement);
  *output_statistics = statistics;
  *output_count = count;
  return true;
}

/**
 * Frees memory allocated for extension statistics.
 */
void db_free_extension_stats(extension_statistic_t *statistics, size_t count) {
  if (!statistics) return;
  for (size_t i = 0; i < count; i++)
    free(statistics[i].extension);
  free(statistics);
}

/**
 * Retrieves records of files with the highest space savings.
 */
bool db_get_top_saved(db_t *database, size_t limit, top_saved_file_t **output_top_files, size_t *output_count) {
  if (!database || !database->connection || !output_top_files || !output_count) return false;
  *output_top_files = NULL;
  *output_count = 0;

  const char *sql = "SELECT "
                    "  path, "
                    "  size, "
                    "  physical_bytes, "
                    "  (size - physical_bytes) AS saved, "
                    "  CASE WHEN size > 0 THEN CAST((size - physical_bytes) AS REAL) / CAST(size AS REAL) "
                    "ELSE 0.0 END "
                    "FROM files "
                    "WHERE size > physical_bytes "
                    "ORDER BY saved DESC LIMIT ?1;";

  sqlite3_stmt *statement = NULL;
  if (sqlite3_prepare_v2(database->connection, sql, -1, &statement, NULL) != SQLITE_OK) return false;
  sqlite3_bind_int64(statement, 1, (sqlite3_int64)limit);

  top_saved_file_t *top_files = malloc(limit * sizeof(top_saved_file_t));
  if (!top_files) {
    sqlite3_finalize(statement);
    return false;
  }

  size_t count = 0;
  while (count < limit && sqlite3_step(statement) == SQLITE_ROW) {
    top_saved_file_t *top_file = &top_files[count++];
    top_file->path = safe_strdup((const char *)sqlite3_column_text(statement, 0));
    top_file->logical_bytes = sqlite3_column_int64(statement, 1);
    top_file->physical_bytes = sqlite3_column_int64(statement, 2);
    top_file->saved_bytes = sqlite3_column_int64(statement, 3);
    top_file->ratio = sqlite3_column_double(statement, 4);
  }

  sqlite3_finalize(statement);
  *output_top_files = top_files;
  *output_count = count;
  return true;
}

/**
 * Frees memory allocated for top saved file records.
 */
void db_free_top_saved(top_saved_file_t *top_files, size_t count) {
  if (!top_files) return;
  for (size_t i = 0; i < count; i++)
    free(top_files[i].path);
  free(top_files);
}
