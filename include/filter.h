#ifndef FSCOMP_FILTER_H
#define FSCOMP_FILTER_H

#include "config.h"

#include <stdbool.h>
#include <sys/stat.h>

typedef enum {
  FILTER_ACTION_PROCESS = 0,
  FILTER_SKIP_INCOMPRESSIBLE_EXTENSION,
  FILTER_SKIP_EXCLUDED_PATTERN,
  FILTER_SKIP_HIDDEN_SYSTEM,
  FILTER_SKIP_TOO_SMALL,
  FILTER_SKIP_SPECIAL_FILE,
} filter_result_t;

/**
 * Returns a human-readable explanation for a filter evaluation decision.
 */
const char *filter_result_string(filter_result_t result);

/**
 * Determines whether a file path should be processed or skipped.
 */
filter_result_t filter_evaluate(const config_t *config, const char *path, const struct stat *file_stat);

/**
 * Extracts the file extension pointer from a path without the leading dot.
 */
const char *filter_get_extension(const char *path);

/**
 * Checks whether a file extension matches the configured incompressible list.
 */
bool filter_is_incompressible_extension(const config_t *config, const char *extension);

/**
 * Checks whether a directory entry matches exclude patterns or system directories.
 */
bool filter_should_skip_directory(const config_t *config, const char *directory_name, const char *full_path);

#endif /* FSCOMP_FILTER_H */
