#define _POSIX_C_SOURCE 200809L
#include "scanner_targets.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/**
 * Returns whether a path is a strict descendant of a directory path.
 */
static bool path_is_inside_directory(const char *path, const char *directory_path) {
  size_t directory_length = strlen(directory_path);
  if (directory_length == 1 && directory_path[0] == '/') return path[0] == '/';
  if (strncmp(path, directory_path, directory_length) != 0) return false;
  return path[directory_length] == '/';
}

/**
 * Resolves target paths and removes targets already covered by a directory.
 */
bool scanner_targets_prepare(const config_t *config, scanner_target_t **output_targets, size_t *output_target_count) {
  scanner_target_t *targets = calloc(config->target_count, sizeof(*targets));
  if (!targets) return false;

  size_t target_count = 0;
  for (size_t config_index = 0; config_index < config->target_count; config_index++) {
    char resolved_path[PATH_MAX];
    const char *configured_path = config->targets[config_index];
    char *target_path = realpath(configured_path, resolved_path) ? strdup(resolved_path) : strdup(configured_path);
    if (!target_path) goto error;

    struct stat target_stat;
    bool is_directory = false;
    if (stat(target_path, &target_stat) == 0) is_directory = S_ISDIR(target_stat.st_mode) != 0;
    bool target_is_covered = false;
    for (size_t target_index = 0; target_index < target_count;) {
      scanner_target_t *existing_target = &targets[target_index];
      if (strcmp(target_path, existing_target->path) == 0
          || (existing_target->is_directory && path_is_inside_directory(target_path, existing_target->path))) {
        target_is_covered = true;
        break;
      }
      if (is_directory && path_is_inside_directory(existing_target->path, target_path)) {
        free(existing_target->path);
        targets[target_index] = targets[--target_count];
        continue;
      }
      target_index++;
    }

    if (target_is_covered) {
      free(target_path);
      continue;
    }
    targets[target_count++] = (scanner_target_t){.path = target_path, .is_directory = is_directory};
  }

  *output_targets = targets;
  *output_target_count = target_count;
  return true;

error:
  scanner_targets_free(targets, target_count);
  return false;
}

/**
 * Releases prepared target paths and their containing array.
 */
void scanner_targets_free(scanner_target_t *targets, size_t target_count) {
  for (size_t index = 0; index < target_count; index++)
    free(targets[index].path);
  free(targets);
}
