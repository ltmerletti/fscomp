#ifndef FSCOMP_DB_H
#define FSCOMP_DB_H

#include "constants.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  char *path;
  int64_t size;
  int64_t modification_time;
  char sha256[SHA256_HEX_STRING_LENGTH];
  int64_t physical_bytes;
  int64_t compressed_bytes;
  int64_t last_compressed_at;
  char *compressor;
  char *status;
} file_record_t;

typedef struct {
  int64_t size;
  int64_t modification_time;
  char sha256[SHA256_HEX_STRING_LENGTH];
} file_metadata_t;

typedef struct {
  int64_t run_id;
  int64_t started_at;
  int64_t completed_at;
  int64_t files_scanned;
  int64_t files_compressed;
  int64_t bytes_saved;
  int64_t duration_milliseconds;
} run_record_t;

typedef struct {
  int64_t total_files;
  int64_t total_compressed;
  int64_t total_incompressible;
  int64_t total_logical_bytes;
  int64_t total_physical_bytes;
  int64_t total_bytes_saved;
} database_summary_t;

typedef struct {
  char *extension;
  int64_t file_count;
  int64_t logical_bytes;
  int64_t physical_bytes;
  int64_t saved_bytes;
} extension_statistic_t;

typedef struct {
  char *path;
  int64_t logical_bytes;
  int64_t physical_bytes;
  int64_t saved_bytes;
  double ratio;
} top_saved_file_t;

typedef struct db_context db_t;

typedef enum {
  DB_LOOKUP_ERROR = -1,
  DB_LOOKUP_NOT_FOUND,
  DB_LOOKUP_FOUND,
} db_lookup_result_t;

/**
 * Opens or creates the SQLite tracking database at the given path.
 */
db_t *db_open(const char *database_path);

/**
 * Opens a concurrent read-only connection to the same SQLite database.
 */
db_t *db_open_read_only(const db_t *database);

/**
 * Finalizes prepared statements and closes the database connection.
 */
void db_close(db_t *database);

/**
 * Starts a database transaction.
 */
bool db_begin(db_t *database);

/**
 * Commits the active database transaction.
 */
bool db_commit(db_t *database);

/**
 * Rolls back the active database transaction.
 */
bool db_rollback(db_t *database);

/**
 * Looks up only the fields needed to decide whether a file changed.
 */
db_lookup_result_t db_get_file_metadata(db_t *database, const char *path, file_metadata_t *output_metadata);

/**
 * Inserts or updates a file record in the database.
 */
bool db_upsert_file(db_t *database, const file_record_t *record);

/**
 * Updates the modification time for a file whose content hash still matches.
 */
bool db_update_file_modification_time(db_t *database, const char *path, int64_t modification_time);

/**
 * Inserts a completed scan run summary record.
 */
bool db_record_run(db_t *database, const run_record_t *run);

/**
 * Calculates total file count and disk savings across all tracked files.
 */
bool db_get_summary(db_t *database, database_summary_t *output_summary);

/**
 * Queries the most recent scan run records up to the given limit.
 */
bool db_get_recent_runs(db_t *database, size_t limit, run_record_t **output_runs, size_t *output_count);

/**
 * Frees an array of run records returned by db_get_recent_runs.
 */
void db_free_recent_runs(run_record_t *runs, size_t count);

/**
 * Computes file counts and space savings grouped by file extension.
 */
bool db_get_extension_stats(db_t *database, extension_statistic_t **output_statistics, size_t *output_count);

/**
 * Frees an array of extension statistics returned by db_get_extension_stats.
 */
void db_free_extension_stats(extension_statistic_t *statistics, size_t count);

/**
 * Queries the files with the largest disk savings up to the given limit.
 */
bool db_get_top_saved(db_t *database, size_t limit, top_saved_file_t **output_top_files, size_t *output_count);

/**
 * Frees an array of top saved file entries returned by db_get_top_saved.
 */
void db_free_top_saved(top_saved_file_t *top_files, size_t count);

#endif /* FSCOMP_DB_H */
