#include "filter.h"

#include "constants.h"

#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/**
 * Returns a human-readable explanation for a filter evaluation decision.
 */
const char *filter_result_string(filter_result_t result) {
  switch (result) {
  case FILTER_ACTION_PROCESS: return "process file";
  case FILTER_SKIP_INCOMPRESSIBLE_EXTENSION: return "skip: extension is an already-compressed format";
  case FILTER_SKIP_EXCLUDED_PATTERN: return "skip: matches an exclusion pattern";
  case FILTER_SKIP_HIDDEN_SYSTEM: return "skip: system metadata or hidden file";
  case FILTER_SKIP_TOO_SMALL: return "skip: size is below the minimum threshold";
  case FILTER_SKIP_SPECIAL_FILE: return "skip: not a regular file";
  default: return "unknown";
  }
}

/**
 * Extracts the file extension pointer from a path without the leading dot.
 */
const char *filter_get_extension(const char *path) {
  if (!path) return NULL;
  const char *last_slash = strrchr(path, '/');
  const char *filename = last_slash ? last_slash + 1 : path;
  const char *dot = strrchr(filename, '.');

  if (!dot || dot == filename || dot[1] == '\0') return NULL;
  return dot + 1;
}

/**
 * Checks whether a file extension matches the configured incompressible list.
 */
bool filter_is_incompressible_extension(const config_t *config, const char *extension) {
  if (!config || !extension || config->incompressible_count == 0) return false;

  for (size_t i = 0; i < config->incompressible_count; i++)
    if (config->incompressible_extensions[i] && strcasecmp(extension, config->incompressible_extensions[i]) == 0)
      return true;
  return false;
}

/**
 * Checks whether a path or filename matches any configured exclude pattern.
 */
static bool matches_exclude_patterns(const config_t *config, const char *name, const char *path) {
  if (!config || !config->excludes || config->exclude_count == 0) return false;

  for (size_t i = 0; i < config->exclude_count; i++) {
    const char *pattern = config->excludes[i];
    if (!pattern || pattern[0] == '\0') continue;
    if (strstr(path, pattern) != NULL) return true;
    if (name && fnmatch(pattern, name, 0) == 0) return true;
    if (fnmatch(pattern, path, 0) == 0) return true;
  }
  return false;
}

/**
 * Determines whether a file path should be processed or skipped.
 */
filter_result_t filter_evaluate(const config_t *config, const char *path, const struct stat *file_stat) {
  if (!path || !file_stat) return FILTER_SKIP_SPECIAL_FILE;

  if (!S_ISREG(file_stat->st_mode)) return FILTER_SKIP_SPECIAL_FILE;

  if (config && config->database_path) {
    size_t database_path_length = strlen(config->database_path);
    if (strncmp(path, config->database_path, database_path_length) == 0) {
      const char suffix_character = path[database_path_length];
      if (suffix_character == '\0' || suffix_character == '-' || suffix_character == '.')
        return FILTER_SKIP_SPECIAL_FILE;
    }
  }

  const char *last_slash = strrchr(path, '/');
  const char *filename = last_slash ? last_slash + 1 : path;
  if (filename[0] == '.') {
    if (strcmp(filename, ".DS_Store") == 0
        || strncmp(filename, APPLE_DOUBLE_FILE_PREFIX, APPLE_DOUBLE_FILE_PREFIX_LENGTH) == 0
        || strcmp(filename, ".localized") == 0) {
      return FILTER_SKIP_HIDDEN_SYSTEM;
    }
  }

  if (matches_exclude_patterns(config, filename, path)) return FILTER_SKIP_EXCLUDED_PATTERN;

  if (config && file_stat->st_size < config->min_size_bytes) return FILTER_SKIP_TOO_SMALL;

  const char *extension = filter_get_extension(path);
  if (extension && filter_is_incompressible_extension(config, extension)) return FILTER_SKIP_INCOMPRESSIBLE_EXTENSION;

  return FILTER_ACTION_PROCESS;
}

/**
 * Checks whether a directory entry matches exclude patterns or system directories.
 */
bool filter_should_skip_directory(const config_t *config, const char *directory_name, const char *full_path) {
  if (!directory_name) return true;

  if (strcmp(directory_name, ".git") == 0 || strcmp(directory_name, ".Trash") == 0
      || strcmp(directory_name, ".Spotlight-V100") == 0 || strcmp(directory_name, ".fseventsd") == 0
      || strcmp(directory_name, ".DocumentRevisions-V100") == 0) {
    return true;
  }

  return matches_exclude_patterns(config, directory_name, full_path);
}
