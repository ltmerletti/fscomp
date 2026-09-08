#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "scanner.h"

#include "compressor.h"
#include "constants.h"
#include "filter.h"
#include "hasher.h"
#include "queue.h"
#include "report.h"
#include "scanner_targets.h"

#include <fts.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_scan_interrupted = 0;
static pthread_mutex_t g_database_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_progress_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_file_mutexes[FILE_LOCK_STRIPE_COUNT];
static pthread_once_t g_file_mutex_once_control = PTHREAD_ONCE_INIT;

typedef struct {
  int64_t last_progress_time_milliseconds;
  int pending_database_records;
  bool database_failed;
  bool is_interactive_terminal;
  bool quiet;
  bool silent;
  const char *operation_name;
} scan_execution_context_t;

typedef struct {
  task_queue_t *work_queue;
  const config_t *config;
  db_t *read_database;
  db_t *write_database;
  const scan_options_t *options;
  scan_statistics_t *statistics;
  scan_execution_context_t *context;
  bool *worker_failed;
  int64_t *pending_task_count;
} scan_worker_context_t;

typedef enum {
  SCAN_TASK_DIRECTORY,
  SCAN_TASK_FILE_BATCH,
} scan_task_type_t;

typedef struct {
  scan_task_type_t type;
} scan_task_t;

typedef struct {
  scan_task_t base;
  char path[];
} directory_task_t;

typedef struct {
  char *path;
  struct stat file_stat;
  char file_hash[SHA256_HEX_STRING_LENGTH];
  pthread_mutex_t *file_lock;
} file_work_item_t;

typedef struct {
  scan_task_t base;
  size_t file_count;
  file_work_item_t files[FILE_TASK_BATCH_CAPACITY];
} file_batch_task_t;

/**
 * Signal handler that marks the active scan interrupted on SIGINT or SIGTERM.
 */
static void handle_interrupt_signal(int signal_number) {
  (void)signal_number;
  g_scan_interrupted = 1;
}

/**
 * Atomically increments a statistic shared by scan workers.
 */
static void increment_statistic(int64_t *field, int64_t amount) {
  (void)__atomic_fetch_add(field, amount, __ATOMIC_RELAXED);
}

/**
 * Atomically reads a statistic while scan workers may update it.
 */
static int64_t read_statistic(const int64_t *field) {
  return __atomic_load_n(field, __ATOMIC_RELAXED);
}

/**
 * Initializes the fixed lock stripes used to serialize duplicate file tasks.
 */
static void initialize_file_mutexes(void) {
  for (size_t index = 0; index < FILE_LOCK_STRIPE_COUNT; index++)
    pthread_mutex_init(&g_file_mutexes[index], NULL);
}

/**
 * Maps a device and inode pair to a stable lock stripe.
 */
static pthread_mutex_t *get_file_mutex(uint64_t device_id, uint64_t inode_number) {
  uint64_t file_identity = device_id ^ inode_number;
  return &g_file_mutexes[file_identity % FILE_LOCK_STRIPE_COUNT];
}

/**
 * Returns the monotonic clock time in milliseconds.
 */
static int64_t current_time_milliseconds(void) {
  struct timespec time_spec;
  clock_gettime(CLOCK_MONOTONIC, &time_spec);
  return ((int64_t)time_spec.tv_sec * MILLISECONDS_PER_SECOND) + (time_spec.tv_nsec / NANOSECONDS_PER_MILLISECOND);
}

/**
 * Shortens long file paths with an ellipsis to fit within terminal width limits.
 */
static void truncate_display_path(const char *source_path, char *destination_buffer, size_t buffer_size,
                                  size_t maximum_length) {
  if (!source_path || !destination_buffer || buffer_size == 0) return;

  size_t source_length = strlen(source_path);
  if (source_length <= maximum_length && source_length < buffer_size) {
    strncpy(destination_buffer, source_path, buffer_size - 1);
    destination_buffer[buffer_size - 1] = 0;
    return;
  }

  const char ellipsis[] = "...";
  size_t ellipsis_length = sizeof(ellipsis) - 1;
  if (maximum_length <= ellipsis_length || buffer_size <= ellipsis_length + 1) {
    strncpy(destination_buffer, ellipsis, buffer_size - 1);
    destination_buffer[buffer_size - 1] = 0;
    return;
  }

  size_t tail_length = maximum_length - ellipsis_length;
  if (tail_length >= buffer_size - ellipsis_length) tail_length = buffer_size - ellipsis_length - 1;

  const char *tail_start = source_path + (source_length - tail_length);
  const char *next_slash = strchr(tail_start, '/');
  if (next_slash && (size_t)(next_slash - tail_start) < 12) tail_start = next_slash;

  snprintf(destination_buffer, buffer_size, "%s%s", ellipsis, tail_start);
}

/**
 * Updates the live progress line on interactive terminals or prints periodic logs.
 */
static void render_progress_update(scan_execution_context_t *context, const scan_statistics_t *statistics,
                                   const char *current_path, bool force_render) {
  if (!context || !statistics || context->quiet || context->silent) return;

  pthread_mutex_lock(&g_progress_mutex);

  int64_t now_milliseconds = current_time_milliseconds();
  if (!force_render
      && (now_milliseconds - context->last_progress_time_milliseconds < PROGRESS_UPDATE_INTERVAL_MILLISECONDS)) {
    pthread_mutex_unlock(&g_progress_mutex);
    return;
  }
  context->last_progress_time_milliseconds = now_milliseconds;

  char saved_string[FORMATTED_BUFFER_CAPACITY];
  int64_t files_scanned = read_statistic(&statistics->files_scanned);
  int64_t files_compressed = read_statistic(&statistics->files_compressed);
  report_format_bytes(read_statistic(&statistics->bytes_saved), saved_string, sizeof(saved_string));

  if (context->is_interactive_terminal) {
    char display_path[PROGRESS_LINE_BUFFER_CAPACITY] = {0};
    if (current_path)
      truncate_display_path(current_path, display_path, sizeof(display_path), PROGRESS_PATH_DISPLAY_LIMIT);
    printf("\r\033[K[%s: %" PRId64 " scanned | %" PRId64 " compressed | %s saved] %s", context->operation_name,
           files_scanned, files_compressed, saved_string, display_path);
    fflush(stdout);
  } else if (files_scanned > 0 && (files_scanned % PROGRESS_NON_TTY_LOG_INTERVAL == 0 || force_render)) {
    printf("[%s] %" PRId64 " files scanned, %" PRId64 " compressed, %s saved...\n", context->operation_name,
           files_scanned, files_compressed, saved_string);
    fflush(stdout);
  }
  pthread_mutex_unlock(&g_progress_mutex);
}

/**
 * Prints a notice when a file has been newly compressed and saved space.
 */
static void notify_file_compressed(scan_execution_context_t *context, const char *file_path, int64_t bytes_saved) {
  if (!context || context->quiet || context->silent) return;
  char saved_string[FORMATTED_BUFFER_CAPACITY];
  report_format_bytes(bytes_saved, saved_string, sizeof(saved_string));

  pthread_mutex_lock(&g_progress_mutex);
  if (context->is_interactive_terminal) printf("\r\033[K");
  printf("  [COMPRESSED] %s, saved %s\n", file_path, saved_string);
  fflush(stdout);
  pthread_mutex_unlock(&g_progress_mutex);
}

/**
 * Upserts a file record to SQLite and commits in periodic batches.
 */
static bool finish_database_write(db_t *database, bool write_succeeded, scan_execution_context_t *context) {
  if (!write_succeeded) {
    __atomic_store_n(&context->database_failed, true, __ATOMIC_RELAXED);
    return false;
  }

  context->pending_database_records++;
  if (context->pending_database_records < DATABASE_COMMIT_BATCH_INTERVAL) return true;

  if (!db_commit(database) || !db_begin(database)) {
    __atomic_store_n(&context->database_failed, true, __ATOMIC_RELAXED);
    return false;
  }
  context->pending_database_records = 0;
  return true;
}

/**
 * Upserts a file record to SQLite and commits in periodic batches.
 */
static bool record_file_and_batch_commit(db_t *database, const file_record_t *record,
                                         scan_execution_context_t *context) {
  if (!database || !record || !context) return false;
  pthread_mutex_lock(&g_database_mutex);
  if (__atomic_load_n(&context->database_failed, __ATOMIC_RELAXED)) {
    pthread_mutex_unlock(&g_database_mutex);
    return false;
  }
  bool write_succeeded = finish_database_write(database, db_upsert_file(database, record), context);
  pthread_mutex_unlock(&g_database_mutex);
  return write_succeeded;
}

/**
 * Updates one verified file's modification time and participates in the write batch.
 */
static bool update_file_modification_time_and_batch_commit(db_t *database, const char *path, int64_t modification_time,
                                                           scan_execution_context_t *context) {
  if (!database || !path || !context) return false;
  pthread_mutex_lock(&g_database_mutex);
  if (__atomic_load_n(&context->database_failed, __ATOMIC_RELAXED)) {
    pthread_mutex_unlock(&g_database_mutex);
    return false;
  }
  bool write_succeeded =
    finish_database_write(database, db_update_file_modification_time(database, path, modification_time), context);
  pthread_mutex_unlock(&g_database_mutex);
  return write_succeeded;
}

/**
 * Initializes execution context state for directory traversal and logging.
 */
static void init_scan_context(scan_execution_context_t *context, const scan_options_t *options,
                              int64_t start_time_milliseconds) {
  memset(context, 0, sizeof(*context));
  context->last_progress_time_milliseconds = start_time_milliseconds;
  context->is_interactive_terminal = (options && !options->quiet && !options->silent) ? isatty(STDOUT_FILENO) : false;
  context->quiet = options ? options->quiet : false;
  context->silent = options ? options->silent : false;
  context->operation_name = (options && options->dry_run) ? "Scan" : "Run";
}

/**
 * Handles a file that the filesystem already stores in compressed form.
 */
static void process_precompressed_file(db_t *database, const char *path, const struct stat *file_stat,
                                       const scan_options_t *options, scan_statistics_t *statistics,
                                       scan_execution_context_t *context) {
  if (options->dry_run) {
    if (options->verbose && !options->quiet && !options->silent)
      printf("  [DRY RUN] %s: already compressed, size %" PRId64 " bytes\n", path, (int64_t)file_stat->st_size);
    render_progress_update(context, statistics, path, false);
    return;
  }

  increment_statistic(&statistics->files_unchanged_stat, 1);
  int64_t physical_bytes = compressor_get_physical_bytes(file_stat);

  file_record_t precompressed_record;
  memset(&precompressed_record, 0, sizeof(precompressed_record));
  precompressed_record.path = (char *)path;
  precompressed_record.size = file_stat->st_size;
  precompressed_record.modification_time = file_stat->st_mtime;
  strncpy(precompressed_record.sha256, "precompressed", sizeof(precompressed_record.sha256) - 1);
  precompressed_record.physical_bytes = physical_bytes;
  precompressed_record.compressed_bytes = physical_bytes;
  precompressed_record.last_compressed_at = (int64_t)file_stat->st_mtime;
  precompressed_record.compressor = "system";
  precompressed_record.status = "compressed";

  if (options->verbose) printf("  [UNCHANGED] %s: already compressed on filesystem\n", path);
  else render_progress_update(context, statistics, path, false);

  if (database) record_file_and_batch_commit(database, &precompressed_record, context);
}

/**
 * Applies the cheap checks, database lookup, and hash check for one file.
 * A true result leaves the file lock held for the caller.
 */
static bool prepare_file_for_compression(scan_worker_context_t *worker_context, file_work_item_t *file) {
  const config_t *config = worker_context->config;
  db_t *read_database = worker_context->read_database;
  db_t *database = worker_context->write_database;
  const scan_options_t *options = worker_context->options;
  scan_statistics_t *statistics = worker_context->statistics;
  scan_execution_context_t *context = worker_context->context;
  const char *path = file->path;
  const struct stat *file_stat = &file->file_stat;

  if (g_scan_interrupted) return false;

  increment_statistic(&statistics->files_scanned, 1);

  filter_result_t filter_result = filter_evaluate(config, path, file_stat);
  if (filter_result != FILTER_ACTION_PROCESS) {
    increment_statistic(&statistics->files_skipped_filter, 1);
    if (options->verbose) printf("  [SKIP] %s: %s\n", path, filter_result_string(filter_result));
    else render_progress_update(context, statistics, path, false);
    return false;
  }

  increment_statistic(&statistics->logical_bytes_processed, (int64_t)file_stat->st_size);

  file_metadata_t existing_metadata;
  db_lookup_result_t lookup_result =
    read_database ? db_get_file_metadata(read_database, path, &existing_metadata) : DB_LOOKUP_NOT_FOUND;
  if (lookup_result == DB_LOOKUP_ERROR) {
    __atomic_store_n(&context->database_failed, true, __ATOMIC_RELAXED);
    increment_statistic(&statistics->files_failed, 1);
    return false;
  }
  bool found_in_database = lookup_result == DB_LOOKUP_FOUND;

  if (found_in_database && !options->rehash_all) {
    if (existing_metadata.size == (int64_t)file_stat->st_size
        && existing_metadata.modification_time == (int64_t)file_stat->st_mtime) {
      increment_statistic(&statistics->files_unchanged_stat, 1);
      if (options->verbose) printf("  [UNCHANGED] %s: size and modification time match database\n", path);
      else render_progress_update(context, statistics, path, false);
      return false;
    }
  }

  bool already_compressed = compressor_is_file_compressed(file_stat);
  if (already_compressed && !options->rehash_all) {
    process_precompressed_file(database, path, file_stat, options, statistics, context);
    return false;
  }

  pthread_once(&g_file_mutex_once_control, initialize_file_mutexes);
  pthread_mutex_t *file_lock = get_file_mutex((uint64_t)file_stat->st_dev, (uint64_t)file_stat->st_ino);
  pthread_mutex_lock(file_lock);

  struct stat refreshed_stat;
  if (stat(path, &refreshed_stat) != 0) {
    increment_statistic(&statistics->files_failed, 1);
    pthread_mutex_unlock(file_lock);
    return false;
  }
  file->file_stat = refreshed_stat;
  file_stat = &file->file_stat;

  if (compressor_is_file_compressed(file_stat) && !options->rehash_all) {
    process_precompressed_file(database, path, file_stat, options, statistics, context);
    pthread_mutex_unlock(file_lock);
    return false;
  }

  char file_hash[SHA256_HEX_STRING_LENGTH] = {0};
  if (!hasher_file_sha256(path, file_hash)) {
    increment_statistic(&statistics->files_failed, 1);
    if (options->verbose) fprintf(stderr, "  [ERROR] %s: could not compute sha256\n", path);
    render_progress_update(context, statistics, path, false);
    pthread_mutex_unlock(file_lock);
    return false;
  }

  if (found_in_database && strcmp(existing_metadata.sha256, file_hash) == 0) {
    increment_statistic(&statistics->files_unchanged_hash, 1);
    if (options->verbose) printf("  [VERIFIED] %s: hash matches %s\n", path, file_hash);
    else render_progress_update(context, statistics, path, false);
    if (!options->dry_run && database)
      update_file_modification_time_and_batch_commit(database, path, file_stat->st_mtime, context);
    pthread_mutex_unlock(file_lock);
    return false;
  }

  if (options->dry_run) {
    if (!options->quiet && !options->silent)
      printf("  [DRY RUN] %s: eligible to compress, size %" PRId64 " bytes\n", path, (int64_t)file_stat->st_size);
    increment_statistic(&statistics->files_compressed, 1);
    render_progress_update(context, statistics, path, false);
    pthread_mutex_unlock(file_lock);
    return false;
  }

  memcpy(file->file_hash, file_hash, sizeof(file->file_hash));
  file->file_lock = file_lock;
  return true;
}

/**
 * Records the result of an afsctool compression attempt.
 */
static void record_compression_result(scan_worker_context_t *worker_context, const file_work_item_t *file,
                                      const compress_outcome_t *compression_outcome) {
  const config_t *config = worker_context->config;
  db_t *database = worker_context->write_database;
  const scan_options_t *options = worker_context->options;
  scan_statistics_t *statistics = worker_context->statistics;
  scan_execution_context_t *context = worker_context->context;

  file_record_t new_record;
  memset(&new_record, 0, sizeof(new_record));
  new_record.path = file->path;
  new_record.size = file->file_stat.st_size;
  new_record.modification_time = file->file_stat.st_mtime;
  strncpy(new_record.sha256, file->file_hash, sizeof(new_record.sha256) - 1);
  new_record.last_compressed_at = (int64_t)time(NULL);
  new_record.compressor = config->compressor;

  if (compression_outcome->result == COMPRESS_RESULT_COMPRESSED) {
    increment_statistic(&statistics->files_compressed, 1);
    increment_statistic(&statistics->bytes_saved, compression_outcome->bytes_saved);
    new_record.physical_bytes = compression_outcome->physical_bytes_after;
    new_record.compressed_bytes = compression_outcome->physical_bytes_after;
    new_record.status = "compressed";
    notify_file_compressed(context, file->path, compression_outcome->bytes_saved);
  } else if (compression_outcome->result == COMPRESS_RESULT_INCOMPRESSIBLE) {
    increment_statistic(&statistics->files_incompressible, 1);
    new_record.physical_bytes = compression_outcome->physical_bytes_after;
    new_record.compressed_bytes = compression_outcome->physical_bytes_after;
    new_record.status = "incompressible";
    if (options->verbose) printf("  [UNCHANGED] %s: afsctool reported no savings\n", file->path);
    else render_progress_update(context, statistics, file->path, false);
  } else {
    increment_statistic(&statistics->files_failed, 1);
    new_record.physical_bytes = compressor_get_physical_bytes(&file->file_stat);
    new_record.compressed_bytes = new_record.physical_bytes;
    new_record.status = "failed";
    if (options->verbose) printf("  [FAILED] %s: %s\n", file->path, compression_outcome->error_message);
    else render_progress_update(context, statistics, file->path, false);
  }

  if (database) record_file_and_batch_commit(database, &new_record, context);
}

/**
 * Compresses prepared files and records each result.
 */
static void compress_prepared_files(scan_worker_context_t *worker_context, file_work_item_t * const *prepared_files,
                                    size_t prepared_file_count) {
  if (prepared_file_count == 0) return;

  const char *paths[FILE_TASK_BATCH_CAPACITY];
  compress_outcome_t compression_outcomes[FILE_TASK_BATCH_CAPACITY];
  for (size_t index = 0; index < prepared_file_count; index++)
    paths[index] = prepared_files[index]->path;

  int compression_worker_count = 0;
  if (prepared_file_count >= MINIMUM_PARALLEL_COMPRESSION_FILE_COUNT) {
    compression_worker_count = worker_context->options->worker_count;
    if (compression_worker_count > MAXIMUM_FLAT_DIRECTORY_COMPRESSION_WORKER_COUNT)
      compression_worker_count = MAXIMUM_FLAT_DIRECTORY_COMPRESSION_WORKER_COUNT;
  }
  compressor_compress(worker_context->config, compression_worker_count, paths, prepared_file_count,
                      compression_outcomes);

  for (size_t index = 0; index < prepared_file_count; index++) {
    record_compression_result(worker_context, prepared_files[index], &compression_outcomes[index]);
    if (prepared_files[index]->file_lock) pthread_mutex_unlock(prepared_files[index]->file_lock);
  }
}

/**
 * Processes one file through the normal single-file afsctool path.
 */
static void process_single_file(scan_worker_context_t *worker_context, const char *path, const struct stat *file_stat) {
  file_work_item_t file = {.path = (char *)path, .file_stat = *file_stat};
  if (!prepare_file_for_compression(worker_context, &file)) return;
  file_work_item_t *prepared_file = &file;
  compress_prepared_files(worker_context, &prepared_file, 1);
}

/**
 * Allocates a directory task with its path in the same memory block.
 */
static directory_task_t *create_directory_task(const char *path) {
  if (!path) return NULL;

  size_t path_length = strlen(path) + 1;
  directory_task_t *task = malloc(sizeof(*task) + path_length);
  if (!task) return NULL;
  task->base.type = SCAN_TASK_DIRECTORY;
  memcpy(task->path, path, path_length);
  return task;
}

/**
 * Completes one queued or active task and shuts down the pool when no work remains.
 */
static void complete_pending_task(scan_worker_context_t *worker_context) {
  int64_t previous_count = __atomic_fetch_sub(worker_context->pending_task_count, 1, __ATOMIC_ACQ_REL);
  if (previous_count == 1) queue_signal_shutdown(worker_context->work_queue);
}

static bool process_path_entries(scan_worker_context_t *worker_context, const char *path);

/**
 * Processes and releases every file stored in a batch task.
 */
static void process_file_batch(scan_worker_context_t *worker_context, file_batch_task_t *batch) {
  file_work_item_t *prepared_files[FILE_TASK_BATCH_CAPACITY];
  size_t prepared_file_count = 0;

  for (size_t index = 0; index < batch->file_count; index++) {
    file_work_item_t *file = &batch->files[index];
    if (prepare_file_for_compression(worker_context, file)) {
      /* Do not hold inode stripes while waiting for the afsctool batch lock. */
      pthread_mutex_unlock(file->file_lock);
      file->file_lock = NULL;
      prepared_files[prepared_file_count++] = file;
    }
  }
  compress_prepared_files(worker_context, prepared_files, prepared_file_count);

  for (size_t index = 0; index < batch->file_count; index++)
    free(batch->files[index].path);
  free(batch);
}

/**
 * Publishes a file batch, or processes it inline when the queue is full.
 */
static void dispatch_file_batch(scan_worker_context_t *worker_context, file_batch_task_t *batch) {
  (void)__atomic_fetch_add(worker_context->pending_task_count, 1, __ATOMIC_ACQ_REL);
  if (queue_try_push(worker_context->work_queue, batch)) return;

  process_file_batch(worker_context, batch);
  complete_pending_task(worker_context);
}

/**
 * Publishes a child directory, or processes it inline when the queue is full.
 */
static bool dispatch_child_path(scan_worker_context_t *worker_context, const char *path) {
  directory_task_t *directory_task = create_directory_task(path);
  if (!directory_task) return false;

  (void)__atomic_fetch_add(worker_context->pending_task_count, 1, __ATOMIC_ACQ_REL);
  if (queue_try_push(worker_context->work_queue, directory_task)) return true;

  bool processing_succeeded = process_path_entries(worker_context, directory_task->path);
  free(directory_task);
  complete_pending_task(worker_context);
  return processing_succeeded;
}

/**
 * Traverses inline, or publishes child directories when a worker queue is active.
 */
static bool process_path_entries(scan_worker_context_t *worker_context, const char *path) {
  char *path_arguments[] = {(char *)path, NULL};
  FTS *file_tree = fts_open(path_arguments, FTS_COMFOLLOW | FTS_PHYSICAL | FTS_NOCHDIR, NULL);
  if (!file_tree) {
    if (worker_context->options->verbose) perror(path);
    return false;
  }

  bool root_accessible = true;
  bool processing_succeeded = true;
  bool parallel = worker_context->work_queue != NULL;
  bool batch_regular_files = false;
  file_batch_task_t *file_batch = NULL;
  FTSENT *entry = NULL;
  while (!g_scan_interrupted && (entry = fts_read(file_tree)) != NULL) {
    if (entry->fts_level == 0) {
      if (entry->fts_info == FTS_F) {
        process_single_file(worker_context, entry->fts_path, entry->fts_statp);
      } else if (entry->fts_info == FTS_D && parallel) {
        size_t regular_file_count = 0;
        for (FTSENT *child = fts_children(file_tree, 0); child; child = child->fts_link) {
          if (child->fts_info == FTS_F && ++regular_file_count >= FILE_TASK_BATCH_CAPACITY) {
            batch_regular_files = true;
            break;
          }
        }
      } else if (entry->fts_info == FTS_DNR || entry->fts_info == FTS_ERR || entry->fts_info == FTS_NS) {
        root_accessible = false;
        if (worker_context->options->verbose)
          fprintf(stderr, "Cannot access %s: %s\n", entry->fts_path, strerror(entry->fts_errno));
      }
      continue;
    }

    if (entry->fts_info == FTS_D) {
      if (filter_should_skip_directory(worker_context->config, entry->fts_name, entry->fts_path)) {
        fts_set(file_tree, entry, FTS_SKIP);
        continue;
      }
      if (parallel) {
        if (!dispatch_child_path(worker_context, entry->fts_path)) processing_succeeded = false;
        fts_set(file_tree, entry, FTS_SKIP);
      }
    } else if (entry->fts_info == FTS_F) {
      if (!batch_regular_files) {
        process_single_file(worker_context, entry->fts_path, entry->fts_statp);
        continue;
      }

      if (!file_batch) {
        file_batch = calloc(1, sizeof(*file_batch));
        if (!file_batch) {
          processing_succeeded = false;
          break;
        }
        file_batch->base.type = SCAN_TASK_FILE_BATCH;
      }
      file_work_item_t *candidate = &file_batch->files[file_batch->file_count];
      candidate->path = strdup(entry->fts_path);
      if (!candidate->path) {
        processing_succeeded = false;
        break;
      }
      candidate->file_stat = *entry->fts_statp;
      file_batch->file_count++;
      if (file_batch->file_count == FILE_TASK_BATCH_CAPACITY) {
        dispatch_file_batch(worker_context, file_batch);
        file_batch = NULL;
      }
    } else if (entry->fts_info == FTS_DNR || entry->fts_info == FTS_ERR || entry->fts_info == FTS_NS) {
      if (entry->fts_level == 0) root_accessible = false;
      if (worker_context->options->verbose)
        fprintf(stderr, "Cannot access %s: %s\n", entry->fts_path, strerror(entry->fts_errno));
    }
  }

  if (file_batch) process_file_batch(worker_context, file_batch);
  fts_close(file_tree);
  return root_accessible && processing_succeeded && !g_scan_interrupted;
}

/**
 * Processes directory tasks until the pending-work counter shuts down the queue.
 */
static void *scan_worker(void *argument) {
  scan_worker_context_t *worker_context = argument;
  void *queued_task;
  pthread_once(&g_file_mutex_once_control, initialize_file_mutexes);
  while (queue_pop(worker_context->work_queue, &queued_task)) {
    scan_task_t *task = queued_task;
    if (task->type == SCAN_TASK_DIRECTORY) {
      directory_task_t *directory_task = (directory_task_t *)task;
      if (!g_scan_interrupted && !process_path_entries(worker_context, directory_task->path))
        __atomic_store_n(worker_context->worker_failed, true, __ATOMIC_RELAXED);
      free(directory_task);
    } else {
      process_file_batch(worker_context, (file_batch_task_t *)task);
    }
    complete_pending_task(worker_context);
  }
  return NULL;
}

/**
 * Runs the scan or compression workflow across all configured target paths.
 */
bool scanner_execute(const config_t *config, db_t *database, const scan_options_t *options,
                     scan_statistics_t *output_statistics) {
  if (!config || !options || !output_statistics) return false;
  memset(output_statistics, 0, sizeof(*output_statistics));

  int worker_count = options->worker_count > DEFAULT_WORKER_COUNT ? options->worker_count : DEFAULT_WORKER_COUNT;
  if (worker_count > MAXIMUM_WORKER_COUNT) worker_count = MAXIMUM_WORKER_COUNT;

  int64_t start_time_milliseconds = current_time_milliseconds();
  int64_t start_time_unix = (int64_t)time(NULL);

  scan_execution_context_t context;
  init_scan_context(&context, options, start_time_milliseconds);

  g_scan_interrupted = 0;
  struct sigaction signal_action;
  memset(&signal_action, 0, sizeof(signal_action));
  signal_action.sa_handler = handle_interrupt_signal;
  sigemptyset(&signal_action.sa_mask);

  struct sigaction previous_sigint;
  struct sigaction previous_sigterm;
  sigaction(SIGINT, &signal_action, &previous_sigint);
  sigaction(SIGTERM, &signal_action, &previous_sigterm);

  bool execution_succeeded = config->target_count > 0;
  scanner_target_t *targets = NULL;
  size_t target_count = 0;
  if (execution_succeeded) execution_succeeded = scanner_targets_prepare(config, &targets, &target_count);
  bool database_transaction_open = false;
  if (database && !options->dry_run) {
    database_transaction_open = db_begin(database);
    if (!database_transaction_open) {
      context.database_failed = true;
      execution_succeeded = false;
    }
  }
  bool worker_failed = false;
  int64_t pending_task_count = 1; /* Main-thread sentinel prevents shutdown while targets are queued. */
  task_queue_t work_queue;
  task_queue_t *active_work_queue = NULL;
  pthread_t worker_threads[MAXIMUM_WORKER_COUNT];
  scan_worker_context_t worker_contexts[MAXIMUM_WORKER_COUNT];
  int started_worker_count = 0;
  scan_worker_context_t common_worker_context = {
    .work_queue = NULL,
    .config = config,
    .read_database = database,
    .write_database = database,
    .options = options,
    .statistics = output_statistics,
    .context = &context,
    .worker_failed = &worker_failed,
    .pending_task_count = &pending_task_count,
  };

  if (execution_succeeded && worker_count > DEFAULT_WORKER_COUNT) {
    execution_succeeded = queue_initialize(&work_queue, DEFAULT_QUEUE_CAPACITY);
    if (execution_succeeded) {
      active_work_queue = &work_queue;
      common_worker_context.work_queue = active_work_queue;
      while (started_worker_count < worker_count) {
        worker_contexts[started_worker_count] = common_worker_context;
        if (database) {
          worker_contexts[started_worker_count].read_database = db_open_read_only(database);
          if (!worker_contexts[started_worker_count].read_database) break;
        }
        int thread_result = pthread_create(&worker_threads[started_worker_count], NULL, scan_worker,
                                           &worker_contexts[started_worker_count]);
        if (thread_result != 0) {
          fprintf(stderr, "Failed to start scan worker: %s\n", strerror(thread_result));
          if (database) db_close(worker_contexts[started_worker_count].read_database);
          break;
        }
        started_worker_count++;
      }
      execution_succeeded = started_worker_count == worker_count;
    }
  }

  if (config->target_count == 0) {
    fprintf(stderr, "No targets configured. Pass target directories as arguments.\n");
  } else if (execution_succeeded) {
    for (size_t index = 0; index < target_count; index++) {
      if (g_scan_interrupted) break;
      const char *target = targets[index].path;
      if (options->verbose) printf("Scanning target: %s\n", target);
      bool target_succeeded;
      if (active_work_queue) {
        directory_task_t *directory_task = create_directory_task(target);
        target_succeeded = directory_task != NULL;
        if (target_succeeded) {
          (void)__atomic_fetch_add(&pending_task_count, 1, __ATOMIC_ACQ_REL);
          target_succeeded = queue_push(active_work_queue, directory_task);
          if (!target_succeeded) {
            free(directory_task);
            complete_pending_task(&common_worker_context);
          }
        }
      } else {
        target_succeeded = process_path_entries(&common_worker_context, target);
      }
      if (!target_succeeded) {
        execution_succeeded = false;
        break;
      }
    }
  }

  if (active_work_queue) {
    complete_pending_task(&common_worker_context);
    if (!execution_succeeded || g_scan_interrupted) queue_signal_shutdown(active_work_queue);
    for (int index = 0; index < started_worker_count; index++) {
      pthread_join(worker_threads[index], NULL);
      if (database) db_close(worker_contexts[index].read_database);
    }
    queue_destroy(active_work_queue);
    if (__atomic_load_n(&worker_failed, __ATOMIC_RELAXED)) execution_succeeded = false;
  }
  scanner_targets_free(targets, target_count);
  if (context.is_interactive_terminal && !options->verbose) {
    printf("\r\033[K");
    fflush(stdout);
  }

  if (g_scan_interrupted && !context.silent)
    printf("\nScan interrupted by user. Saved partial progress to database.\n");

  if (database_transaction_open) {
    if (!db_commit(database)) context.database_failed = true;
    context.pending_database_records = 0;
  }

  sigaction(SIGINT, &previous_sigint, NULL);
  sigaction(SIGTERM, &previous_sigterm, NULL);

  int64_t end_time_milliseconds = current_time_milliseconds();
  int64_t end_time_unix = (int64_t)time(NULL);
  output_statistics->duration_milliseconds = end_time_milliseconds - start_time_milliseconds;

  if (database && !options->dry_run && output_statistics->files_scanned > 0) {
    run_record_t run;
    memset(&run, 0, sizeof(run));
    run.started_at = start_time_unix;
    run.completed_at = end_time_unix;
    run.files_scanned = output_statistics->files_scanned;
    run.files_compressed = output_statistics->files_compressed;
    run.bytes_saved = output_statistics->bytes_saved;
    run.duration_milliseconds = output_statistics->duration_milliseconds;
    if (!db_record_run(database, &run)) context.database_failed = true;
  }

  return execution_succeeded && !g_scan_interrupted && !__atomic_load_n(&context.database_failed, __ATOMIC_RELAXED);
}
