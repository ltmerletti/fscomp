#ifndef FSCOMP_SCANNER_H
#define FSCOMP_SCANNER_H

#include "config.h"
#include "db.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  bool dry_run;
  bool rehash_all;
  bool verbose;
  bool quiet;
  bool silent;
  int worker_count;
} scan_options_t;

typedef struct {
  int64_t files_scanned;
  int64_t files_skipped_filter;
  int64_t files_unchanged_stat;
  int64_t files_unchanged_hash;
  int64_t files_compressed;
  int64_t files_incompressible;
  int64_t files_failed;
  int64_t logical_bytes_processed;
  int64_t bytes_saved;
  int64_t duration_milliseconds;
} scan_statistics_t;

/**
 * Runs the scan or compression workflow across all configured target paths.
 */
bool scanner_execute(const config_t *config, db_t *database, const scan_options_t *options,
                     scan_statistics_t *output_statistics);


#endif /* FSCOMP_SCANNER_H */
