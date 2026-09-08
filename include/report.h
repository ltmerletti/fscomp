#ifndef FSCOMP_REPORT_H
#define FSCOMP_REPORT_H

#include "db.h"
#include "scanner.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Formats a raw byte count into human-readable units such as KB, MB, or GB.
 */
void report_format_bytes(int64_t bytes, char *output_buffer, size_t buffer_size);

/**
 * Prints a summary of files scanned, compressed, skipped, and bytes saved.
 */
void report_print_scan_summary(const scan_statistics_t *statistics, bool dry_run);

/**
 * Prints high-level statistics from the state database.
 */
void report_print_status(db_t *database);

/**
 * Prints run history, savings grouped by extension, and top saved files.
 */
void report_print_report(db_t *database);

#endif /* FSCOMP_REPORT_H */
