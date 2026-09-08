#define _POSIX_C_SOURCE 200809L
#include "config.h"

#include "cJSON.h"
#include "constants.h"

#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *DEFAULT_INCOMPRESSIBLE_EXTENSIONS[] = {
  "zip",  "gz",   "bz2",  "xz",   "7z",  "zst",  "tgz", "tbz2", "txz", "rar",  "mp4",  "m4v",  "mkv",  "mov",  "avi",
  "wmv",  "flv",  "webm", "mp3",  "aac", "flac", "wav", "m4a",  "ogg", "opus", "wma",  "jpg",  "jpeg", "png",  "gif",
  "webp", "heic", "heif", "avif", "dmg", "pkg",  "iso", "img",  "jar", "apk",  "docx", "xlsx", "pptx", "epub", "pdf"};
static const size_t DEFAULT_INCOMPRESSIBLE_COUNT =
  sizeof(DEFAULT_INCOMPRESSIBLE_EXTENSIONS) / sizeof(DEFAULT_INCOMPRESSIBLE_EXTENSIONS[0]);

static const char *DEFAULT_EXCLUDES[] = {"/.git/", "/.Trash/", ".DS_Store"};
static const size_t DEFAULT_EXCLUDES_COUNT = sizeof(DEFAULT_EXCLUDES) / sizeof(DEFAULT_EXCLUDES[0]);

static void free_string_array(char **array, size_t count) {
  if (!array) return;
  for (size_t i = 0; i < count; i++)
    free(array[i]);
  free(array);
}

static char **read_json_string_array(const cJSON *json_array, size_t *output_count, bool expand_paths) {
  if (!cJSON_IsArray(json_array)) return NULL;
  size_t count = (size_t)cJSON_GetArraySize(json_array);
  if (count == 0) return NULL;

  char **entries = calloc(count, sizeof(char *));
  if (!entries) return NULL;

  size_t valid_count = 0;
  for (size_t i = 0; i < count; i++) {
    cJSON *element = cJSON_GetArrayItem(json_array, (int)i);
    if (cJSON_IsString(element) && element->valuestring)
      entries[valid_count++] = expand_paths ? config_expand_path(element->valuestring) : strdup(element->valuestring);
  }
  *output_count = valid_count;
  return entries;
}

static void load_json_string_array(const cJSON *json, const char *key, char ***destination, size_t *destination_count,
                                   bool expand_paths) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
  size_t count = 0;
  char **entries = read_json_string_array(item, &count, expand_paths);
  if (!entries) return;

  free_string_array(*destination, *destination_count);
  *destination = entries;
  *destination_count = count;
}

static void load_json_string(const cJSON *json, const char *key, char **destination, bool expand_path) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
  if (!cJSON_IsString(item) || !item->valuestring) return;

  free(*destination);
  *destination = expand_path ? config_expand_path(item->valuestring) : strdup(item->valuestring);
}

char *config_expand_path(const char *path) {
  if (!path) return NULL;
  if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
    const char *home_directory = getenv("HOME");
    if (!home_directory) {
      struct passwd *password_entry = getpwuid(getuid());
      if (password_entry) home_directory = password_entry->pw_dir;
    }
    if (home_directory) {
      size_t home_length = strlen(home_directory);
      size_t path_length = strlen(path + 1);
      char *expanded_path = malloc(home_length + path_length + 1);
      if (!expanded_path) return NULL;
      strcpy(expanded_path, home_directory);
      strcat(expanded_path, path + 1);
      return expanded_path;
    }
  }
  return strdup(path);
}

char *config_get_default_path(void) {
  return config_expand_path("~/.config/fscomp/config.json");
}

static char *find_afsctool_path(void) {
  const char *candidate_paths[] = {
    "/opt/homebrew/bin/afsctool",
    "/usr/local/bin/afsctool",
    "/usr/bin/afsctool",
  };
  for (size_t i = 0; i < sizeof(candidate_paths) / sizeof(candidate_paths[0]); i++)
    if (access(candidate_paths[i], X_OK) == 0) return strdup(candidate_paths[i]);
  return strdup("afsctool");
}

config_t *config_create_default(void) {
  config_t *config = calloc(1, sizeof(*config));
  if (!config) return NULL;

  config->targets = NULL;
  config->target_count = 0;

  config->excludes = calloc(DEFAULT_EXCLUDES_COUNT, sizeof(char *));
  config->exclude_count = DEFAULT_EXCLUDES_COUNT;
  for (size_t i = 0; i < DEFAULT_EXCLUDES_COUNT; i++)
    config->excludes[i] = strdup(DEFAULT_EXCLUDES[i]);

  config->compressor = strdup("LZFSE");
  config->zlib_level = DEFAULT_ZLIB_LEVEL;
  config->min_size_bytes = DEFAULT_MINIMUM_FILE_SIZE_BYTES;
  config->min_saving_percentage = DEFAULT_MINIMUM_SAVINGS_PERCENTAGE;
  config->worker_count = DEFAULT_WORKER_COUNT;

  config->incompressible_extensions = calloc(DEFAULT_INCOMPRESSIBLE_COUNT, sizeof(char *));
  config->incompressible_count = DEFAULT_INCOMPRESSIBLE_COUNT;
  for (size_t i = 0; i < DEFAULT_INCOMPRESSIBLE_COUNT; i++)
    config->incompressible_extensions[i] = strdup(DEFAULT_INCOMPRESSIBLE_EXTENSIONS[i]);

  config->database_path = config_expand_path("~/.config/fscomp/state.db");
  config->afsctool_path = find_afsctool_path();

  return config;
}

void config_free(config_t *config) {
  if (!config) return;
  free_string_array(config->targets, config->target_count);
  free_string_array(config->excludes, config->exclude_count);
  free_string_array(config->incompressible_extensions, config->incompressible_count);
  free(config->compressor);
  free(config->database_path);
  free(config->afsctool_path);
  free(config);
}

config_t *config_load(const char *path) {
  char *resolved_path = path ? config_expand_path(path) : config_get_default_path();
  if (!resolved_path) return config_create_default();

  FILE *file_handle = fopen(resolved_path, "rb");
  if (!file_handle) {
    free(resolved_path);
    return config_create_default();
  }

  fseek(file_handle, 0, SEEK_END);
  int64_t file_size = (int64_t)ftell(file_handle);
  fseek(file_handle, 0, SEEK_SET);

  if (file_size <= 0 || file_size > MAXIMUM_CONFIG_FILE_SIZE_BYTES) {
    fclose(file_handle);
    free(resolved_path);
    return config_create_default();
  }

  char *file_buffer = malloc(file_size + 1);
  if (!file_buffer) {
    fclose(file_handle);
    free(resolved_path);
    return config_create_default();
  }

  size_t bytes_read = fread(file_buffer, 1, file_size, file_handle);
  file_buffer[bytes_read] = '\0';
  fclose(file_handle);
  free(resolved_path);

  cJSON *json = cJSON_Parse(file_buffer);
  free(file_buffer);
  if (!json) return config_create_default();

  config_t *config = config_create_default();
  if (!config) {
    cJSON_Delete(json);
    return NULL;
  }

  load_json_string_array(json, "targets", &config->targets, &config->target_count, true);
  load_json_string_array(json, "excludes", &config->excludes, &config->exclude_count, false);
  load_json_string(json, "compressor", &config->compressor, false);

  cJSON *level_item = cJSON_GetObjectItemCaseSensitive(json, "zlib_level");
  if (cJSON_IsNumber(level_item)) config->zlib_level = level_item->valueint;

  cJSON *minimum_size_item = cJSON_GetObjectItemCaseSensitive(json, "min_size_bytes");
  if (cJSON_IsNumber(minimum_size_item)) config->min_size_bytes = (int64_t)minimum_size_item->valuedouble;

  cJSON *minimum_percentage_item = cJSON_GetObjectItemCaseSensitive(json, "min_saving_percentage");
  if (cJSON_IsNumber(minimum_percentage_item)) config->min_saving_percentage = minimum_percentage_item->valueint;

  cJSON *worker_count_item = cJSON_GetObjectItemCaseSensitive(json, "worker_count");
  if (cJSON_IsNumber(worker_count_item)) {
    int configured_worker_count = worker_count_item->valueint;
    if (worker_count_item->valuedouble == configured_worker_count && configured_worker_count >= DEFAULT_WORKER_COUNT
        && configured_worker_count <= MAXIMUM_WORKER_COUNT)
      config->worker_count = configured_worker_count;
  }

  load_json_string_array(json, "incompressible_extensions", &config->incompressible_extensions,
                         &config->incompressible_count, false);
  load_json_string(json, "db_path", &config->database_path, true);
  load_json_string(json, "afsctool_path", &config->afsctool_path, true);

  cJSON_Delete(json);
  return config;
}

static void ensure_parent_directory(const char *path) {
  if (!path) return;
  char *path_copy = strdup(path);
  if (!path_copy) return;

  for (char *cursor = path_copy + 1; *cursor; cursor++) {
    if (*cursor == '/') {
      *cursor = '\0';
      mkdir(path_copy, DIRECTORY_CREATION_PERMISSIONS);
      *cursor = '/';
    }
  }
  free(path_copy);
}

static void add_json_string_array(cJSON *json, const char *key, char * const *entries, size_t count) {
  cJSON *array = entries ? cJSON_CreateStringArray((const char * const *)entries, (int)count) : cJSON_CreateArray();
  cJSON_AddItemToObject(json, key, array);
}

bool config_save(const config_t *config, const char *path) {
  if (!config) return false;
  char *resolved_path = path ? config_expand_path(path) : config_get_default_path();
  if (!resolved_path) return false;

  ensure_parent_directory(resolved_path);

  cJSON *json = cJSON_CreateObject();
  if (!json) {
    free(resolved_path);
    return false;
  }

  add_json_string_array(json, "targets", config->targets, config->target_count);
  add_json_string_array(json, "excludes", config->excludes, config->exclude_count);
  cJSON_AddStringToObject(json, "compressor", config->compressor ? config->compressor : "LZFSE");
  cJSON_AddNumberToObject(json, "zlib_level", config->zlib_level);
  cJSON_AddNumberToObject(json, "min_size_bytes", (double)config->min_size_bytes);
  cJSON_AddNumberToObject(json, "min_saving_percentage", config->min_saving_percentage);
  cJSON_AddNumberToObject(json, "worker_count", config->worker_count);

  add_json_string_array(json, "incompressible_extensions", config->incompressible_extensions,
                        config->incompressible_count);
  cJSON_AddStringToObject(json, "db_path", config->database_path ? config->database_path : "");
  cJSON_AddStringToObject(json, "afsctool_path", config->afsctool_path ? config->afsctool_path : "afsctool");

  char *rendered = cJSON_Print(json);
  cJSON_Delete(json);

  if (!rendered) {
    free(resolved_path);
    return false;
  }

  FILE *file_handle = fopen(resolved_path, "w");
  if (!file_handle) {
    free(rendered);
    free(resolved_path);
    return false;
  }

  fputs(rendered, file_handle);
  fclose(file_handle);
  free(rendered);
  free(resolved_path);
  return true;
}

bool config_add_target(config_t *config, const char *path) {
  if (!config || !path || path[0] == '\0') return false;

  char *expanded_path = config_expand_path(path);
  if (!expanded_path) return false;

  char resolved_path[PATH_MAX];
  char *stored_path = NULL;
  if (realpath(expanded_path, resolved_path) != NULL) stored_path = strdup(resolved_path);
  else stored_path = strdup(expanded_path);
  free(expanded_path);

  if (!stored_path) return false;

  size_t length = strlen(stored_path);
  if (length > 1 && stored_path[length - 1] == '/') stored_path[length - 1] = '\0';

  for (size_t i = 0; i < config->target_count; i++) {
    if (strcmp(config->targets[i], stored_path) == 0) {
      free(stored_path);
      return false;
    }
  }

  char **new_targets = realloc(config->targets, (config->target_count + 1) * sizeof(char *));
  if (!new_targets) {
    free(stored_path);
    return false;
  }

  config->targets = new_targets;
  config->targets[config->target_count] = stored_path;
  config->target_count++;
  return true;
}

bool config_remove_target(config_t *config, const char *path) {
  if (!config || !path || config->target_count == 0) return false;

  char *expanded_path = config_expand_path(path);
  if (!expanded_path) return false;

  char resolved_path[PATH_MAX];
  const char *lookup_path = expanded_path;
  if (realpath(expanded_path, resolved_path) != NULL) lookup_path = resolved_path;

  size_t match_index = (size_t)-1;
  for (size_t i = 0; i < config->target_count; i++) {
    if (strcmp(config->targets[i], lookup_path) == 0 || strcmp(config->targets[i], expanded_path) == 0
        || strcmp(config->targets[i], path) == 0) {
      match_index = i;
      break;
    }
  }
  free(expanded_path);

  if (match_index == (size_t)-1) return false;

  free(config->targets[match_index]);
  for (size_t i = match_index; i + 1 < config->target_count; i++)
    config->targets[i] = config->targets[i + 1];
  config->target_count--;

  if (config->target_count == 0) {
    free(config->targets);
    config->targets = NULL;
  }
  return true;
}
