#ifndef FSCOMP_SCANNER_TARGETS_H
#define FSCOMP_SCANNER_TARGETS_H

#include "config.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
  char *path;
  bool is_directory;
} scanner_target_t;

bool scanner_targets_prepare(const config_t *config, scanner_target_t **output_targets, size_t *output_target_count);
void scanner_targets_free(scanner_target_t *targets, size_t target_count);

#endif /* FSCOMP_SCANNER_TARGETS_H */
