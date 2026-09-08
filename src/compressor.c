#define _POSIX_C_SOURCE 200809L
#include "compressor.h"

#include "constants.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* Single-file calls may overlap. A parallel batch runs alone to prevent nested afsctool pools. */
static pthread_rwlock_t g_afsctool_process_lock = PTHREAD_RWLOCK_INITIALIZER;

/**
 * Checks whether a file already has the macOS UF_COMPRESSED flag set.
 */
bool compressor_is_file_compressed(const struct stat *file_stat) {
  if (!file_stat) return false;
  return (file_stat->st_flags & UF_COMPRESSED) != 0;
}

/**
 * Calculates physical bytes allocated on disk from stat st_blocks.
 */
int64_t compressor_get_physical_bytes(const struct stat *file_stat) {
  if (!file_stat) return 0;
  return (int64_t)file_stat->st_blocks * BYTES_PER_STAT_BLOCK;
}

static void run_afsctool(const config_t *config, int worker_count, const char * const *paths, size_t path_count,
                         compress_outcome_t *output_outcomes) {
  if (!config || !paths || path_count == 0 || path_count > FILE_TASK_BATCH_CAPACITY || !output_outcomes) return;

  char *argument_vector[FILE_TASK_BATCH_CAPACITY + AFSCTOOL_FIXED_ARGUMENT_CAPACITY] = {0};
  bool path_ready[FILE_TASK_BATCH_CAPACITY] = {false};
  size_t argument_count = 0;

  const char *afsctool_binary = config->afsctool_path ? config->afsctool_path : "afsctool";
  argument_vector[argument_count++] = (char *)afsctool_binary;
  argument_vector[argument_count++] = "-c";

  char worker_count_buffer[OPTION_STRING_CAPACITY];
  if (worker_count > 1) {
    snprintf(worker_count_buffer, sizeof(worker_count_buffer), "-J%d", worker_count);
    argument_vector[argument_count++] = worker_count_buffer;
  }

  const char *compressor_type = config->compressor ? config->compressor : "LZFSE";
  argument_vector[argument_count++] = "-T";
  argument_vector[argument_count++] = (char *)compressor_type;

  char zlib_level_buffer[OPTION_STRING_CAPACITY];
  if (strcasecmp(compressor_type, "ZLIB") == 0 && config->zlib_level >= MINIMUM_ZLIB_LEVEL
      && config->zlib_level <= MAXIMUM_ZLIB_LEVEL) {
    snprintf(zlib_level_buffer, sizeof(zlib_level_buffer), "-%d", config->zlib_level);
    argument_vector[argument_count++] = zlib_level_buffer;
  }

  char percentage_buffer[OPTION_STRING_CAPACITY];
  if (config->min_saving_percentage > 0) {
    argument_vector[argument_count++] = "-s";
    snprintf(percentage_buffer, sizeof(percentage_buffer), "%d", config->min_saving_percentage);
    argument_vector[argument_count++] = percentage_buffer;
  }

  size_t ready_path_count = 0;
  for (size_t index = 0; index < path_count; index++) {
    compress_outcome_t *outcome = &output_outcomes[index];
    memset(outcome, 0, sizeof(*outcome));
    outcome->result = COMPRESS_RESULT_FAILED;

    struct stat stat_before;
    if (!paths[index] || stat(paths[index], &stat_before) != 0) {
      snprintf(outcome->error_message, sizeof(outcome->error_message), "Failed to inspect file before compression");
      continue;
    }
    outcome->logical_bytes = stat_before.st_size;
    outcome->physical_bytes_before = compressor_get_physical_bytes(&stat_before);
    path_ready[index] = true;
    ready_path_count++;
    argument_vector[argument_count++] = (char *)paths[index];
  }
  argument_vector[argument_count] = NULL;

  if (ready_path_count == 0) return;

  posix_spawn_file_actions_t file_actions;
  posix_spawn_file_actions_init(&file_actions);
  posix_spawn_file_actions_addopen(&file_actions, STDOUT_FILENO, "/dev/null", O_WRONLY, POSIX_SPAWN_DEV_NULL_FILE_MODE);
  posix_spawn_file_actions_addopen(&file_actions, STDERR_FILENO, "/dev/null", O_WRONLY, POSIX_SPAWN_DEV_NULL_FILE_MODE);

  pid_t process_id;
  int spawn_result_code = posix_spawnp(&process_id, afsctool_binary, &file_actions, NULL, argument_vector, environ);
  posix_spawn_file_actions_destroy(&file_actions);

  char process_error[ERROR_MESSAGE_CAPACITY] = {0};
  if (spawn_result_code != 0) {
    snprintf(process_error, sizeof(process_error), "posix_spawnp failed with code %d", spawn_result_code);
  } else {
    int process_status = 0;
    pid_t wait_result;
    do {
      wait_result = waitpid(process_id, &process_status, 0);
    } while (wait_result == -1 && errno == EINTR);
    if (wait_result == -1 || !WIFEXITED(process_status) || WEXITSTATUS(process_status) != 0)
      snprintf(process_error, sizeof(process_error), "afsctool exited abnormally");
  }

  if (process_error[0] != '\0') {
    for (size_t index = 0; index < path_count; index++) {
      if (!path_ready[index]) continue;
      snprintf(output_outcomes[index].error_message, sizeof(output_outcomes[index].error_message), "%s", process_error);
    }
    return;
  }

  for (size_t index = 0; index < path_count; index++) {
    if (!path_ready[index]) continue;
    compress_outcome_t *outcome = &output_outcomes[index];
    struct stat stat_after;
    if (stat(paths[index], &stat_after) != 0) {
      outcome->result = COMPRESS_RESULT_FAILED;
      snprintf(outcome->error_message, sizeof(outcome->error_message), "Failed to inspect file after compression");
      continue;
    }

    outcome->physical_bytes_after = compressor_get_physical_bytes(&stat_after);
    if (compressor_is_file_compressed(&stat_after)) {
      outcome->result = COMPRESS_RESULT_COMPRESSED;
      int64_t physical_bytes_after = outcome->physical_bytes_after;
      int64_t physical_bytes_before = outcome->physical_bytes_before;
      int64_t logical_bytes = outcome->logical_bytes;

      if (physical_bytes_before > physical_bytes_after)
        outcome->bytes_saved = physical_bytes_before - physical_bytes_after;
      else if (logical_bytes > physical_bytes_after) outcome->bytes_saved = logical_bytes - physical_bytes_after;
    } else {
      outcome->result = COMPRESS_RESULT_INCOMPRESSIBLE;
    }
  }
}

/**
 * Runs one afsctool process and reports the result for each path.
 */
void compressor_compress(const config_t *config, int worker_count, const char * const *paths, size_t path_count,
                         compress_outcome_t *output_outcomes) {
  bool parallel = worker_count > 1;
  if (parallel) pthread_rwlock_wrlock(&g_afsctool_process_lock);
  else pthread_rwlock_rdlock(&g_afsctool_process_lock);

  run_afsctool(config, worker_count, paths, path_count, output_outcomes);

  pthread_rwlock_unlock(&g_afsctool_process_lock);
}
