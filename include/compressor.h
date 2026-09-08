#ifndef FSCOMP_COMPRESSOR_H
#define FSCOMP_COMPRESSOR_H

#include "config.h"
#include "constants.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

typedef enum {
  COMPRESS_RESULT_COMPRESSED = 0,
  COMPRESS_RESULT_ALREADY_COMPRESSED,
  COMPRESS_RESULT_INCOMPRESSIBLE,
  COMPRESS_RESULT_FAILED
} compress_result_type_t;

typedef struct {
  compress_result_type_t result;
  int64_t logical_bytes;
  int64_t physical_bytes_before;
  int64_t physical_bytes_after;
  int64_t bytes_saved;
  char error_message[ERROR_MESSAGE_CAPACITY];
} compress_outcome_t;

/**
 * Checks whether a file already has the macOS UF_COMPRESSED flag set.
 */
bool compressor_is_file_compressed(const struct stat *file_stat);

/**
 * Calculates physical bytes allocated on disk from stat st_blocks.
 */
int64_t compressor_get_physical_bytes(const struct stat *file_stat);

/**
 * Runs one afsctool process and reports the result for each path.
 */
void compressor_compress(const config_t *config, int worker_count, const char * const *paths, size_t path_count,
                         compress_outcome_t *output_outcomes);

#endif /* FSCOMP_COMPRESSOR_H */
