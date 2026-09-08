#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "compressor.h"
#include "config.h"
#include "constants.h"
#include "db.h"
#include "filter.h"
#include "hasher.h"
#include "queue.h"
#include "scanner.h"

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_PASS(test_name) printf("  pass: %s\n", test_name)

#define TEST_QUEUE_SMALL_CAPACITY 4
#define TEST_QUEUE_CONCURRENCY_CAPACITY 8
#define TEST_PRODUCER_ITEMS_PER_THREAD 50
#define TEST_PRODUCER_THREAD_COUNT 2
#define TEST_CONSUMER_THREAD_COUNT 2
#define TEST_SCAN_WORKER_COUNT 2
#define TEST_CONFIG_WORKER_COUNT 4
#define TEST_CONFIG_PATH "/tmp/fscomp_test_config.json"
#define TEST_FLAT_DIRECTORY_FILE_COUNT 40

#define TEST_REPEAT_COUNT_SINGLE 1
#define TEST_REPEAT_COUNT_LARGE 1200
#define TEST_REPEAT_COUNT_ZIP 2000
#define TEST_REPEAT_COUNT_SMALL 10
#define TEST_REPEAT_COUNT_MODIFIED 1500

/**
 * Writes repeated string content to a new test file on disk.
 */
static void create_test_file(const char *path, const char *content, size_t repeat_count) {
  FILE *file_handle = fopen(path, "wb");
  assert(file_handle != NULL);
  for (size_t i = 0; i < repeat_count; i++)
    fputs(content, file_handle);
  fclose(file_handle);
}

/**
 * Verifies SHA-256 calculation and repeatability on a test file.
 */
static void test_hasher(void) {
  printf("Testing sha256 hasher\n");
  const char *test_path = "/tmp/fscomp_test_hash.txt";
  create_test_file(test_path, "Hello, APFS compression world!\n", TEST_REPEAT_COUNT_SINGLE);

  char output_hash[SHA256_HEX_STRING_LENGTH] = {0};
  bool hash_success = hasher_file_sha256(test_path, output_hash);
  assert(hash_success);
  assert(strlen(output_hash) == (SHA256_HEX_STRING_LENGTH - 1));

  char output_hash_second[SHA256_HEX_STRING_LENGTH] = {0};
  hasher_file_sha256(test_path, output_hash_second);
  assert(strcmp(output_hash, output_hash_second) == 0);

  unlink(test_path);
  TEST_PASS("test_hasher");
}

/**
 * Tests target management, exclusion rules, and incompressible extension matching.
 */
static void test_config_and_filter(void) {
  printf("Testing config and filter logic\n");
  config_t *config = config_create_default();
  assert(config != NULL);
  assert(config->target_count == 0);
  assert(config->min_size_bytes == DEFAULT_MINIMUM_FILE_SIZE_BYTES);
  assert(config->worker_count == DEFAULT_WORKER_COUNT);
  assert(strcmp(config->compressor, "LZFSE") == 0);

  config->worker_count = TEST_CONFIG_WORKER_COUNT;
  assert(config_save(config, TEST_CONFIG_PATH));
  config_t *loaded_config = config_load(TEST_CONFIG_PATH);
  assert(loaded_config != NULL);
  assert(loaded_config->worker_count == TEST_CONFIG_WORKER_COUNT);
  config_free(loaded_config);
  unlink(TEST_CONFIG_PATH);

  /* Test adding and removing tracked target directories. */
  assert(config_add_target(config, "/tmp"));
  assert(config->target_count == 1);
  /* Reject duplicate target directory additions. */
  assert(!config_add_target(config, "/tmp"));
  assert(config->target_count == 1);

  assert(config_remove_target(config, "/tmp"));
  assert(config->target_count == 0);
  /* Return false when attempting to remove a directory that is not tracked. */
  assert(!config_remove_target(config, "/tmp"));

  assert(filter_is_incompressible_extension(config, "zip"));
  assert(filter_is_incompressible_extension(config, "ZIP"));
  assert(filter_is_incompressible_extension(config, "mp4"));
  assert(filter_is_incompressible_extension(config, "jpg"));
  assert(!filter_is_incompressible_extension(config, "txt"));
  assert(!filter_is_incompressible_extension(config, "log"));
  assert(!filter_is_incompressible_extension(config, "json"));

  struct stat file_stat;
  memset(&file_stat, 0, sizeof(file_stat));
  file_stat.st_mode = S_IFREG | TEST_FILE_PERMISSIONS;
  file_stat.st_size = 10000;

  assert(filter_evaluate(config, "/path/to/archive.zip", &file_stat) == FILTER_SKIP_INCOMPRESSIBLE_EXTENSION);
  assert(filter_evaluate(config, "/path/to/video.MP4", &file_stat) == FILTER_SKIP_INCOMPRESSIBLE_EXTENSION);
  assert(filter_evaluate(config, "/repo/.git/objects/123", &file_stat) == FILTER_SKIP_EXCLUDED_PATTERN);
  assert(filter_evaluate(config, "/dir/.DS_Store", &file_stat) == FILTER_SKIP_HIDDEN_SYSTEM);

  file_stat.st_size = 500;
  assert(filter_evaluate(config, "/path/to/small.txt", &file_stat) == FILTER_SKIP_TOO_SMALL);

  file_stat.st_size = 50000;
  assert(filter_evaluate(config, "/path/to/data.txt", &file_stat) == FILTER_ACTION_PROCESS);

  config_free(config);
  TEST_PASS("test_config_and_filter");
}

/**
 * Tests database creation, file record upsert, queries, and summaries.
 */
static void test_db(void) {
  printf("Testing SQLite state database\n");
  const char *test_database_path = "/tmp/fscomp_test.db";
  unlink(test_database_path);

  db_t *database = db_open(test_database_path);
  assert(database != NULL);

  file_record_t record = {
    .path = "/test/path/file1.txt",
    .size = 100000,
    .modification_time = 1234567,
    .sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    .physical_bytes = 12000,
    .compressed_bytes = 12000,
    .last_compressed_at = 1234568,
    .compressor = "LZFSE",
    .status = "compressed",
  };

  assert(db_upsert_file(database, &record));

  db_t *read_database = db_open_read_only(database);
  assert(read_database != NULL);
  file_metadata_t metadata;
  assert(db_get_file_metadata(read_database, record.path, &metadata) == DB_LOOKUP_FOUND);
  assert(metadata.size == record.size);
  assert(metadata.modification_time == record.modification_time);
  assert(strcmp(metadata.sha256, record.sha256) == 0);
  assert(db_get_file_metadata(read_database, "/missing/file", &metadata) == DB_LOOKUP_NOT_FOUND);
  db_close(read_database);

  run_record_t run = {
    .started_at = 1000,
    .completed_at = 1010,
    .files_scanned = 50,
    .files_compressed = 10,
    .bytes_saved = 88000,
    .duration_milliseconds = 10000,
  };
  assert(db_record_run(database, &run));

  run_record_t *recent_runs = NULL;
  size_t run_count = 0;
  assert(db_get_recent_runs(database, DEFAULT_TOP_SAVED_LIMIT, &recent_runs, &run_count));
  assert(run_count == 1);
  assert(recent_runs[0].bytes_saved == 88000);
  db_free_recent_runs(recent_runs, run_count);

  database_summary_t summary;
  assert(db_get_summary(database, &summary));
  assert(summary.total_files == 1);
  assert(summary.total_compressed == 1);
  assert(summary.total_logical_bytes == 100000);
  assert(summary.total_physical_bytes == 12000);
  assert(summary.total_bytes_saved == 88000);

  extension_statistic_t *extension_statistics = NULL;
  size_t extension_count = 0;
  assert(db_get_extension_stats(database, &extension_statistics, &extension_count));
  assert(extension_count == 1);
  assert(strcmp(extension_statistics[0].extension, "txt") == 0);
  assert(extension_statistics[0].saved_bytes == 88000);
  db_free_extension_stats(extension_statistics, extension_count);

  top_saved_file_t *top_files = NULL;
  size_t top_file_count = 0;
  assert(db_get_top_saved(database, DEFAULT_TOP_SAVED_LIMIT, &top_files, &top_file_count));
  assert(top_file_count == 1);
  assert(strcmp(top_files[0].path, record.path) == 0);
  assert(top_files[0].saved_bytes == 88000);
  db_free_top_saved(top_files, top_file_count);

  db_close(database);
  unlink(test_database_path);
  TEST_PASS("test_db");
}

/**
 * Tests directory scanning, dry runs, compression, and incremental change detection.
 */
static void test_incremental_scanner_and_compression(void) {
  printf("Testing scanner, compression, and incremental change detection\n");

  const char *test_directory = "/tmp/fscomp_suite_dir";
  const char *test_database_path = "/tmp/fscomp_suite.db";

  char setup_command[PATH_BUFFER_CAPACITY];
  snprintf(setup_command, sizeof(setup_command), "rm -rf %s %s && mkdir -p %s/subdir", test_directory,
           test_database_path, test_directory);
  system(setup_command);

  char first_file[PATH_BUFFER_CAPACITY];
  char second_file[PATH_BUFFER_CAPACITY];
  char archive_file[PATH_BUFFER_CAPACITY];
  char small_file[PATH_BUFFER_CAPACITY];

  snprintf(first_file, sizeof(first_file), "%s/compressible1.txt", test_directory);
  snprintf(second_file, sizeof(second_file), "%s/subdir/compressible2.log", test_directory);
  snprintf(archive_file, sizeof(archive_file), "%s/archive.zip", test_directory);
  snprintf(small_file, sizeof(small_file), "%s/small.txt", test_directory);

  char git_directory[PATH_BUFFER_CAPACITY];
  char git_file[PATH_BUFFER_CAPACITY];
  snprintf(git_directory, sizeof(git_directory), "%s/.git", test_directory);
  mkdir(git_directory, DIRECTORY_CREATION_PERMISSIONS);
  snprintf(git_file, sizeof(git_file), "%s/ignored.txt", git_directory);
  create_test_file(git_file, "Git directory file should be excluded\n", TEST_REPEAT_COUNT_LARGE);

  /* Verify that virtual environments and caches ARE scanned and compressed. */
  char venv_directory[PATH_BUFFER_CAPACITY];
  char venv_file[PATH_BUFFER_CAPACITY];
  snprintf(venv_directory, sizeof(venv_directory), "%s/.venv", test_directory);
  mkdir(venv_directory, DIRECTORY_CREATION_PERMISSIONS);
  snprintf(venv_file, sizeof(venv_file), "%s/script.py", venv_directory);
  create_test_file(venv_file, "def python_function():\n    return 'repeated compressible code'\n",
                   TEST_REPEAT_COUNT_LARGE);

  create_test_file(first_file, "The quick brown fox jumps over the lazy dog repeatedly! ", TEST_REPEAT_COUNT_LARGE);
  create_test_file(second_file, "Line number 0123456789 logging event data timestamp string\n",
                   TEST_REPEAT_COUNT_LARGE);
  create_test_file(archive_file, "dummy zip file content ", TEST_REPEAT_COUNT_ZIP);
  create_test_file(small_file, "small file content\n", TEST_REPEAT_COUNT_SMALL);

  config_t *config = config_create_default();
  char *targets[2] = {(char *)test_directory, second_file};
  config->targets = targets;
  config->target_count = 1;

  db_t *database = db_open(test_database_path);
  assert(database != NULL);

  scan_options_t dry_run_options = {
    .dry_run = true,
    .verbose = false,
    .rehash_all = false,
    .worker_count = DEFAULT_WORKER_COUNT,
  };
  scan_statistics_t dry_run_statistics;
  bool scan_success = scanner_execute(config, database, &dry_run_options, &dry_run_statistics);
  assert(scan_success);
  assert(dry_run_statistics.files_scanned == 5);
  assert(dry_run_statistics.files_skipped_filter >= 2);
  assert(dry_run_statistics.files_compressed == 3);

  scan_options_t run_options = {
    .dry_run = false,
    .verbose = false,
    .rehash_all = false,
    .worker_count = TEST_SCAN_WORKER_COUNT,
  };
  /* Add a file already covered by the directory target to verify target deduplication. */
  config->target_count = 2;
  scan_statistics_t first_run_statistics;
  scan_success = scanner_execute(config, database, &run_options, &first_run_statistics);
  assert(scan_success);
  assert(first_run_statistics.files_scanned == 5);
  assert(first_run_statistics.files_compressed == 3);
  assert(first_run_statistics.bytes_saved > 0);
  config->target_count = 1;

  struct stat verified_stat;
  stat(first_file, &verified_stat);
  assert(compressor_is_file_compressed(&verified_stat));

  scan_statistics_t second_run_statistics;
  scan_success = scanner_execute(config, database, &run_options, &second_run_statistics);
  assert(scan_success);
  assert(second_run_statistics.files_scanned == 5);
  assert(second_run_statistics.files_unchanged_stat == 3);
  assert(second_run_statistics.files_compressed == 0);

  sleep(1);
  create_test_file(first_file, "MODIFIED content updated with newly added repeated lines!\n",
                   TEST_REPEAT_COUNT_MODIFIED);

  scan_statistics_t third_run_statistics;
  scan_success = scanner_execute(config, database, &run_options, &third_run_statistics);
  assert(scan_success);
  assert(third_run_statistics.files_scanned == 5);
  assert(third_run_statistics.files_unchanged_stat == 2);
  assert(third_run_statistics.files_compressed == 1);

  scan_options_t rehash_options = {
    .dry_run = false,
    .verbose = false,
    .rehash_all = true,
  };
  scan_statistics_t rehash_statistics;
  scan_success = scanner_execute(config, database, &rehash_options, &rehash_statistics);
  assert(scan_success);
  assert(rehash_statistics.files_scanned == 5);
  assert(rehash_statistics.files_unchanged_hash == 3);
  assert(rehash_statistics.files_compressed == 0);

  database_summary_t verified_summary;
  assert(db_get_summary(database, &verified_summary));
  assert(verified_summary.total_compressed == 3);
  assert(verified_summary.total_bytes_saved > 0);

  db_close(database);

  /* Test pre-compressed file detection on a fresh database. */
  char second_db_path[PATH_BUFFER_CAPACITY];
  snprintf(second_db_path, sizeof(second_db_path), "/tmp/fscomp_test_second.db");
  db_t *second_database = db_open(second_db_path);
  assert(second_database != NULL);

  scan_statistics_t precompressed_statistics;
  scan_success = scanner_execute(config, second_database, &run_options, &precompressed_statistics);
  assert(scan_success);
  assert(precompressed_statistics.files_scanned == 5);
  /* Files already compressed on disk are recognized without recompressing. */
  assert(precompressed_statistics.files_compressed == 0);
  assert(precompressed_statistics.files_unchanged_stat == 3);
  /* Test silent and quiet execution options. */
  scan_options_t quiet_options = {
    .dry_run = true,
    .quiet = true,
    .silent = false,
  };
  scan_statistics_t quiet_statistics;
  assert(scanner_execute(config, second_database, &quiet_options, &quiet_statistics));
  assert(quiet_statistics.files_scanned == 5);

  scan_options_t silent_options = {
    .dry_run = true,
    .quiet = false,
    .silent = true,
  };
  scan_statistics_t silent_statistics;
  assert(scanner_execute(config, second_database, &silent_options, &silent_statistics));
  assert(silent_statistics.files_scanned == 5);

  db_close(second_database);

  config->targets = NULL;
  config->target_count = 0;
  config_free(config);

  char cleanup_command[PATH_BUFFER_CAPACITY];
  snprintf(cleanup_command, sizeof(cleanup_command), "rm -rf %s %s %s*", test_directory, test_database_path,
           second_db_path);
  system(cleanup_command);

  TEST_PASS("test_incremental_scanner_and_compression");
}

/**
 * Exercises file batching when one directory contains enough files to occupy the worker pool.
 */
static void test_flat_directory_batching(void) {
  printf("Testing flat directory batching\n");
  const char *test_directory = "/tmp/fscomp_flat_directory";
  mkdir(test_directory, DIRECTORY_CREATION_PERMISSIONS);

  char file_path[PATH_BUFFER_CAPACITY];
  for (size_t index = 0; index < TEST_FLAT_DIRECTORY_FILE_COUNT; index++) {
    snprintf(file_path, sizeof(file_path), "%s/file_%zu.txt", test_directory, index);
    create_test_file(file_path, "flat directory batching test data\n", TEST_REPEAT_COUNT_LARGE);
  }

  config_t *config = config_create_default();
  assert(config != NULL);
  assert(config_add_target(config, test_directory));

  scan_options_t options = {
    .dry_run = true,
    .silent = true,
    .worker_count = TEST_CONFIG_WORKER_COUNT,
  };
  scan_statistics_t statistics;
  assert(scanner_execute(config, NULL, &options, &statistics));
  assert(statistics.files_scanned == TEST_FLAT_DIRECTORY_FILE_COUNT);
  assert(statistics.files_compressed == TEST_FLAT_DIRECTORY_FILE_COUNT);

  options.dry_run = false;
  assert(scanner_execute(config, NULL, &options, &statistics));
  assert(statistics.files_scanned == TEST_FLAT_DIRECTORY_FILE_COUNT);
  assert(statistics.files_compressed == TEST_FLAT_DIRECTORY_FILE_COUNT);
  struct stat compressed_file_stat;
  snprintf(file_path, sizeof(file_path), "%s/file_0.txt", test_directory);
  assert(stat(file_path, &compressed_file_stat) == 0);
  assert(compressor_is_file_compressed(&compressed_file_stat));

  config_free(config);
  for (size_t index = 0; index < TEST_FLAT_DIRECTORY_FILE_COUNT; index++) {
    snprintf(file_path, sizeof(file_path), "%s/file_%zu.txt", test_directory, index);
    unlink(file_path);
  }
  rmdir(test_directory);
  TEST_PASS("test_flat_directory_batching");
}

/**
 * Tests single-threaded task queue initialization, push, pop, and shutdown.
 */
typedef struct {
  char path[PATH_MAX];
  int64_t file_size;
} test_queue_item_t;

static void test_queue_basic(void) {
  printf("Testing task queue basic operations\n");

  task_queue_t queue;
  assert(!queue_initialize(NULL, TEST_QUEUE_SMALL_CAPACITY));
  assert(!queue_initialize(&queue, 0));

  assert(queue_initialize(&queue, TEST_QUEUE_SMALL_CAPACITY));
  assert(queue_get_count(&queue) == 0);

  for (size_t index = 0; index < TEST_QUEUE_SMALL_CAPACITY; index++) {
    test_queue_item_t *item = calloc(1, sizeof(*item));
    assert(item != NULL);
    snprintf(item->path, sizeof(item->path), "/path/test_%zu.txt", index);
    item->file_size = (int64_t)(index + 1) * 1000;
    assert(queue_push(&queue, item));
  }

  assert(queue_get_count(&queue) == TEST_QUEUE_SMALL_CAPACITY);

  test_queue_item_t rejected_item;
  memset(&rejected_item, 0, sizeof(rejected_item));
  assert(!queue_try_push(&queue, &rejected_item));

  for (size_t index = 0; index < TEST_QUEUE_SMALL_CAPACITY; index++) {
    void *queue_item = NULL;
    assert(queue_pop(&queue, &queue_item));
    test_queue_item_t *popped_item = queue_item;
    char expected_path[PATH_MAX];
    snprintf(expected_path, sizeof(expected_path), "/path/test_%zu.txt", index);
    assert(strcmp(popped_item->path, expected_path) == 0);
    assert(popped_item->file_size == (int64_t)(index + 1) * 1000);
    free(popped_item);
  }

  assert(queue_get_count(&queue) == 0);

  assert(queue_try_push(&queue, &rejected_item));
  void *popped_item = NULL;
  assert(queue_pop(&queue, &popped_item));
  assert(popped_item == &rejected_item);

  queue_signal_shutdown(&queue);
  assert(!queue_push(&queue, &rejected_item));
  assert(!queue_try_push(&queue, &rejected_item));
  assert(!queue_pop(&queue, &popped_item));

  queue_destroy(&queue);
  TEST_PASS("test_queue_basic");
}

typedef struct {
  task_queue_t *queue;
  size_t thread_id;
  int items_to_produce;
} producer_context_t;

typedef struct {
  task_queue_t *queue;
  int total_popped_count;
  int64_t total_popped_bytes;
} consumer_context_t;

/**
 * Worker thread routine that pushes items into the shared task queue.
 */
static void *producer_worker(void *argument) {
  producer_context_t *context = (producer_context_t *)argument;
  for (int item_index = 0; item_index < context->items_to_produce; item_index++) {
    test_queue_item_t *item = calloc(1, sizeof(*item));
    assert(item != NULL);
    snprintf(item->path, sizeof(item->path), "/item_%zu_%d.dat", context->thread_id, item_index);
    item->file_size = 100;
    assert(queue_push(context->queue, item));
  }
  return NULL;
}

/**
 * Worker thread routine that consumes items from the shared task queue.
 */
static void *consumer_worker(void *argument) {
  consumer_context_t *context = (consumer_context_t *)argument;
  void *queue_item = NULL;
  while (queue_pop(context->queue, &queue_item)) {
    test_queue_item_t *item = queue_item;
    context->total_popped_count++;
    context->total_popped_bytes += item->file_size;
    free(item);
  }
  return NULL;
}

/**
 * Tests concurrent producer-consumer workers operating on the task queue.
 */
static void test_queue_concurrency(void) {
  printf("Testing task queue multithreaded concurrency\n");

  task_queue_t queue;
  assert(queue_initialize(&queue, TEST_QUEUE_CONCURRENCY_CAPACITY));

  pthread_t producers[TEST_PRODUCER_THREAD_COUNT];
  producer_context_t producer_contexts[TEST_PRODUCER_THREAD_COUNT];

  pthread_t consumers[TEST_CONSUMER_THREAD_COUNT];
  consumer_context_t consumer_contexts[TEST_CONSUMER_THREAD_COUNT];

  for (size_t index = 0; index < TEST_CONSUMER_THREAD_COUNT; index++) {
    consumer_contexts[index].queue = &queue;
    consumer_contexts[index].total_popped_count = 0;
    consumer_contexts[index].total_popped_bytes = 0;
    assert(pthread_create(&consumers[index], NULL, consumer_worker, &consumer_contexts[index]) == 0);
  }

  for (size_t index = 0; index < TEST_PRODUCER_THREAD_COUNT; index++) {
    producer_contexts[index].queue = &queue;
    producer_contexts[index].thread_id = index;
    producer_contexts[index].items_to_produce = TEST_PRODUCER_ITEMS_PER_THREAD;
    assert(pthread_create(&producers[index], NULL, producer_worker, &producer_contexts[index]) == 0);
  }

  for (size_t index = 0; index < TEST_PRODUCER_THREAD_COUNT; index++)
    pthread_join(producers[index], NULL);

  queue_signal_shutdown(&queue);

  int total_popped = 0;
  int64_t total_bytes = 0;
  for (size_t index = 0; index < TEST_CONSUMER_THREAD_COUNT; index++) {
    pthread_join(consumers[index], NULL);
    total_popped += consumer_contexts[index].total_popped_count;
    total_bytes += consumer_contexts[index].total_popped_bytes;
  }

  int expected_total_items = TEST_PRODUCER_THREAD_COUNT * TEST_PRODUCER_ITEMS_PER_THREAD;
  assert(total_popped == expected_total_items);
  assert(total_bytes == (int64_t)expected_total_items * 100);

  queue_destroy(&queue);
  TEST_PASS("test_queue_concurrency");
}

/**
 * Test runner entry point executing all test suites.
 */
int main(void) {
  printf("Starting fscomp test suite\n\n");
  test_hasher();
  test_config_and_filter();
  test_db();
  test_incremental_scanner_and_compression();
  test_flat_directory_batching();
  test_queue_basic();
  test_queue_concurrency();
  printf("\nAll tests passed.\n");
  return 0;
}
