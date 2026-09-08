#ifndef FSCOMP_CONSTANTS_H
#define FSCOMP_CONSTANTS_H

#include <sys/stat.h>

/* Directory permissions 0755 for ~/.config/fscomp. */
#define DIRECTORY_CREATION_PERMISSIONS (S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)

/* File permissions 0644 used for creating test files. */
#define TEST_FILE_PERMISSIONS (S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH)

/* Mode argument for redirecting output to /dev/null with posix_spawn. Because /dev/null already
 * exists, mode is unused. */
#define POSIX_SPAWN_DEV_NULL_FILE_MODE 0

/* Prefix for macOS AppleDouble metadata files. Skip these files so split resource forks are not
 * compressed. */
#define APPLE_DOUBLE_FILE_PREFIX "._"
#define APPLE_DOUBLE_FILE_PREFIX_LENGTH 2

/* stat measures st_blocks in 512-byte chunks. Multiply st_blocks by 512 to get actual bytes on
 * disk. */
#define BYTES_PER_STAT_BLOCK 512

/* Bytes in one kibibyte, 1024, used for formatting file sizes. */
#define BYTES_PER_KIBIBYTE 1024.0

/* Minimum file size to compress. APFS writes data in 4 KB blocks, so files under 4096 bytes cannot
 * save any disk space. */
#define DEFAULT_MINIMUM_FILE_SIZE_BYTES 4096

/* Maximum size for config.json, 10 MB, to prevent runaway memory use if an invalid file is loaded.
 */
#define MAXIMUM_CONFIG_FILE_SIZE_BYTES (10 * 1024 * 1024)

/* Chunk size, 64 KB, for streaming file contents into SHA-256. */
#define HASHER_CHUNK_BUFFER_SIZE_BYTES 65536

/* Flag in st_flags that macOS sets when a file is compressed through decmpfs. */
#ifndef UF_COMPRESSED
  #define UF_COMPRESSED 0x00000020
#endif

/* String and buffer capacities */
#define SHA256_HEX_STRING_LENGTH 65 /* 64 hex characters plus NUL terminator */
#define HEX_CHARS_PER_BYTE 2 /* Each byte formats into 2 hex characters */
#define HEX_BYTE_BUFFER_SIZE 3 /* 2 hex characters plus NUL terminator for snprintf */
#define PATH_BUFFER_CAPACITY 4096 /* Path buffer size, matching macOS PATH_MAX */
#define FORMATTED_BUFFER_CAPACITY 32 /* Buffer size for readable file sizes like 12.34 MB */
#define TIME_STRING_BUFFER_CAPACITY 64 /* Buffer size for timestamps like 2026-09-04 12:00:00 */
#define ERROR_MESSAGE_CAPACITY 256 /* Buffer size for system and compression error messages */
#define AFSCTOOL_FIXED_ARGUMENT_CAPACITY 10 /* Executable, options, and terminating NULL argument */
#define OPTION_STRING_CAPACITY 16 /* Buffer size for short CLI options like -5 */
#define INITIAL_DYNAMIC_ARRAY_CAPACITY \
  16 /* Starting capacity for dynamic arrays before resizing  \
                                           */
#define PROGRESS_LINE_BUFFER_CAPACITY 256 /* Buffer size for the terminal progress line */
#define PROGRESS_PATH_DISPLAY_LIMIT 45 /* Maximum characters displayed for a path in progress */

/* Time conversions */
#define MILLISECONDS_PER_SECOND 1000
#define NANOSECONDS_PER_MILLISECOND 1000000

/* Compression settings for afsctool */
#define DEFAULT_ZLIB_LEVEL 5 /* Default zlib level, from 1 fastest to 9 best compression */
#define MINIMUM_ZLIB_LEVEL 1
#define MAXIMUM_ZLIB_LEVEL 9
#define DEFAULT_MINIMUM_SAVINGS_PERCENTAGE 5 /* Minimum space savings percentage needed to keep compression */

/* Report formatting and limits */
#define PERCENTAGE_MULTIPLIER \
  100.0 /* Multiplier to convert a 0.0 to 1.0 ratio into a percentage                            \
           */
#define MAXIMUM_UNIT_INDEX 5 /* Index of PB, the largest unit in the byte formatting list */
#define DEFAULT_RECENT_RUNS_LIMIT 5 /* Number of recent runs shown in the report */
#define DEFAULT_TOP_SAVED_LIMIT 10 /* Number of top saved files shown in the report */
#define DEFAULT_EXTENSION_STATS_LIMIT 20 /* Number of file extension rows shown in the report */

/* Worker queue and thread limits */
#define DEFAULT_QUEUE_CAPACITY 128 /* Maximum number of pending file tasks */
#define DEFAULT_WORKER_COUNT 1 /* Single-threaded unless --jobs is set */
#define MAXIMUM_WORKER_COUNT 64 /* Upper bound for --jobs */
#define FILE_LOCK_STRIPE_COUNT 64 /* Number of mutexes used to serialize duplicate file tasks */
#define FILE_TASK_BATCH_CAPACITY 32 /* Files grouped into one task when a flat directory needs parallel work */
#define MINIMUM_PARALLEL_COMPRESSION_FILE_COUNT 8 /* Small groups use the normal per-file path */
#define MAXIMUM_FLAT_DIRECTORY_COMPRESSION_WORKER_COUNT 8 /* Upper bound for afsctool batch workers */

/* Progress display intervals */
#define PROGRESS_UPDATE_INTERVAL_MILLISECONDS 100 /* Minimum elapsed time between live terminal status line updates */
#define PROGRESS_NON_TTY_LOG_INTERVAL \
  5000 /* Number of files scanned between progress log prints when stdout is not a TTY */

/* Database batch commit interval */
#define DATABASE_COMMIT_BATCH_INTERVAL 150 /* Commit the open SQLite transaction every 150 files to prevent work loss */

#endif /* FSCOMP_CONSTANTS_H */
