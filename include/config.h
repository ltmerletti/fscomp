#ifndef FSCOMP_CONFIG_H
#define FSCOMP_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  char **targets;
  size_t target_count;

  char **excludes;
  size_t exclude_count;

  char *compressor;
  int32_t zlib_level;
  int64_t min_size_bytes;
  int32_t min_saving_percentage;
  int32_t worker_count;

  char **incompressible_extensions;
  size_t incompressible_count;

  char *database_path;
  char *afsctool_path;
} config_t;

/**
 * Creates a configuration object populated with default settings.
 */
config_t *config_create_default(void);

/**
 * Frees a configuration object and all its allocated strings and arrays.
 */
void config_free(config_t *config);

/**
 * Expands leading tilde in a filesystem path to the user home directory.
 */
char *config_expand_path(const char *path);

/**
 * Returns the default configuration file path in ~/.config/fscomp.
 */
char *config_get_default_path(void);

/**
 * Loads settings from a JSON configuration file on disk.
 */
config_t *config_load(const char *path);

/**
 * Serializes and writes configuration settings to a JSON file on disk.
 */
bool config_save(const config_t *config, const char *path);

/**
 * Adds a directory path to the list of tracked target directories.
 */
bool config_add_target(config_t *config, const char *path);

/**
 * Removes a directory path from the list of tracked target directories.
 */
bool config_remove_target(config_t *config, const char *path);

#endif /* FSCOMP_CONFIG_H */
