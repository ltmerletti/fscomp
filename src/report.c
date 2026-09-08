#include "report.h"

#include "constants.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/**
 * Formats a raw byte count into human-readable units such as KB, MB, or GB.
 */
void report_format_bytes(int64_t bytes, char *output_buffer, size_t buffer_size) {
  if (!output_buffer || buffer_size == 0) return;

  if (bytes < 0) {
    snprintf(output_buffer, buffer_size, "0 B");
    return;
  }

  const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  int unit_index = 0;
  double floating_bytes = (double)bytes;

  while (floating_bytes >= BYTES_PER_KIBIBYTE && unit_index < MAXIMUM_UNIT_INDEX) {
    floating_bytes /= BYTES_PER_KIBIBYTE;
    unit_index++;
  }

  if (unit_index == 0) snprintf(output_buffer, buffer_size, "%" PRId64 " B", bytes);
  else snprintf(output_buffer, buffer_size, "%.2f %s", floating_bytes, units[unit_index]);
}

/**
 * Prints a summary of files scanned, compressed, skipped, and bytes saved.
 */
void report_print_scan_summary(const scan_statistics_t *statistics, bool dry_run) {
  if (!statistics) return;

  char processed_string[FORMATTED_BUFFER_CAPACITY];
  char saved_string[FORMATTED_BUFFER_CAPACITY];
  report_format_bytes(statistics->logical_bytes_processed, processed_string, sizeof(processed_string));
  report_format_bytes(statistics->bytes_saved, saved_string, sizeof(saved_string));

  printf("\n--- %s summary ---\n", dry_run ? "Scan" : "Run");
  printf("  Duration:                  %.2f seconds\n",
         (double)statistics->duration_milliseconds / (double)MILLISECONDS_PER_SECOND);
  printf("  Files scanned:             %" PRId64 "\n", statistics->files_scanned);
  printf("  Files skipped by filter:   %" PRId64 "\n", statistics->files_skipped_filter);
  printf("  Files skipped by stat:     %" PRId64 "\n", statistics->files_unchanged_stat);
  printf("  Files verified by hash:    %" PRId64 "\n", statistics->files_unchanged_hash);
  if (dry_run) {
    printf("  Files to compress:         %" PRId64 "\n", statistics->files_compressed);
  } else {
    printf("  Files compressed:          %" PRId64 "\n", statistics->files_compressed);
    printf("  Files without savings:     %" PRId64 "\n", statistics->files_incompressible);
  }
  printf("  Errors:                    %" PRId64 "\n", statistics->files_failed);
  printf("  Eligible logical size:     %s\n", processed_string);
  if (!dry_run) printf("  Disk space saved:          %s\n", saved_string);
  printf("---------------------------\n\n");
}

/**
 * Prints high-level statistics from the state database.
 */
void report_print_status(db_t *database) {
  if (!database) {
    fprintf(stderr, "Database is not open.\n");
    return;
  }

  database_summary_t summary;
  if (!db_get_summary(database, &summary)) {
    fprintf(stderr, "Failed to read database summary.\n");
    return;
  }

  char logical_string[FORMATTED_BUFFER_CAPACITY];
  char physical_string[FORMATTED_BUFFER_CAPACITY];
  char saved_string[FORMATTED_BUFFER_CAPACITY];
  report_format_bytes(summary.total_logical_bytes, logical_string, sizeof(logical_string));
  report_format_bytes(summary.total_physical_bytes, physical_string, sizeof(physical_string));
  report_format_bytes(summary.total_bytes_saved, saved_string, sizeof(saved_string));

  double percentage_saved = 0.0;
  if (summary.total_logical_bytes > 0) {
    percentage_saved =
      ((double)summary.total_bytes_saved / (double)summary.total_logical_bytes) * PERCENTAGE_MULTIPLIER;
  }

  printf("\n--- Database status ---\n");
  printf("  Tracked files:             %" PRId64 "\n", summary.total_files);
  printf("  Compressed files:          %" PRId64 "\n", summary.total_compressed);
  printf("  Incompressible files:      %" PRId64 "\n", summary.total_incompressible);
  printf("  Logical size:              %s\n", logical_string);
  printf("  Physical size on disk:     %s\n", physical_string);
  printf("  Space saved:               %s, %.1f%%\n", saved_string, percentage_saved);
  printf("-----------------------\n\n");
}

/**
 * Prints run history, savings grouped by extension, and top saved files.
 */
void report_print_report(db_t *database) {
  if (!database) {
    fprintf(stderr, "Database is not open.\n");
    return;
  }

  report_print_status(database);

  run_record_t *runs = NULL;
  size_t run_count = 0;
  if (db_get_recent_runs(database, DEFAULT_RECENT_RUNS_LIMIT, &runs, &run_count) && run_count > 0) {
    printf("Recent runs, latest %zu:\n", run_count);
    printf("  %-6s  %-20s  %-10s  %-10s  %-12s  %-12s\n", "Run ID", "Completed", "Duration", "Scanned", "Compressed",
           "Saved");
    printf("  ----------------------------------------------------------------------------\n");

    for (size_t i = 0; i < run_count; i++) {
      const run_record_t *run = &runs[i];
      char time_buffer[TIME_STRING_BUFFER_CAPACITY] = "unknown";
      time_t timestamp = (time_t)run->completed_at;
      struct tm time_information;
      if (localtime_r(&timestamp, &time_information))
        strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", &time_information);

      char saved_string[FORMATTED_BUFFER_CAPACITY];
      report_format_bytes(run->bytes_saved, saved_string, sizeof(saved_string));

      printf("  %-6" PRId64 "  %-20s  %-9.2fs  %-10" PRId64 "  %-12" PRId64 "  %-12s\n", run->run_id, time_buffer,
             (double)run->duration_milliseconds / (double)MILLISECONDS_PER_SECOND, run->files_scanned,
             run->files_compressed, saved_string);
    }
    printf("\n");
    db_free_recent_runs(runs, run_count);
  }

  extension_statistic_t *extension_statistics = NULL;
  size_t extension_count = 0;
  if (db_get_extension_stats(database, &extension_statistics, &extension_count) && extension_count > 0) {
    printf("Savings by file extension:\n");
    printf("  %-10s  %-8s  %-12s  %-12s  %-12s  %-8s\n", "Ext", "Files", "Logical", "Physical", "Saved", "Ratio");
    printf("  ----------------------------------------------------------------------------\n");

    for (size_t i = 0; i < extension_count; i++) {
      const extension_statistic_t *statistic = &extension_statistics[i];
      char logical_string[FORMATTED_BUFFER_CAPACITY];
      char physical_string[FORMATTED_BUFFER_CAPACITY];
      char saved_string[FORMATTED_BUFFER_CAPACITY];
      report_format_bytes(statistic->logical_bytes, logical_string, sizeof(logical_string));
      report_format_bytes(statistic->physical_bytes, physical_string, sizeof(physical_string));
      report_format_bytes(statistic->saved_bytes, saved_string, sizeof(saved_string));

      double ratio = 0.0;
      if (statistic->logical_bytes > 0)
        ratio = ((double)statistic->saved_bytes / (double)statistic->logical_bytes) * PERCENTAGE_MULTIPLIER;

      printf("  %-10s  %-8" PRId64 "  %-12s  %-12s  %-12s  %5.1f%%\n",
             statistic->extension ? statistic->extension : "none", statistic->file_count, logical_string,
             physical_string, saved_string, ratio);
    }
    printf("\n");
    db_free_extension_stats(extension_statistics, extension_count);
  }

  top_saved_file_t *top_files = NULL;
  size_t top_file_count = 0;
  if (db_get_top_saved(database, DEFAULT_TOP_SAVED_LIMIT, &top_files, &top_file_count) && top_file_count > 0) {
    printf("Top saved files, latest %zu:\n", top_file_count);
    printf("  %-12s  %-8s  %s\n", "Saved", "Ratio", "Path");
    printf("  ----------------------------------------------------------------------------\n");

    for (size_t i = 0; i < top_file_count; i++) {
      const top_saved_file_t *file = &top_files[i];
      char saved_string[FORMATTED_BUFFER_CAPACITY];
      report_format_bytes(file->saved_bytes, saved_string, sizeof(saved_string));
      printf("  %-12s  %5.1f%%   %s\n", saved_string, file->ratio * PERCENTAGE_MULTIPLIER, file->path);
    }
    printf("\n");
    db_free_top_saved(top_files, top_file_count);
  }
}
