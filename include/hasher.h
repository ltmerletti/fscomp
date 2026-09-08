#ifndef FSCOMP_HASHER_H
#define FSCOMP_HASHER_H

#include "constants.h"

#include <stdbool.h>

/**
 * Computes the SHA-256 hash of a file as a 64-character lowercase hex string.
 */
bool hasher_file_sha256(const char *path, char output_hex[SHA256_HEX_STRING_LENGTH]);

#endif /* FSCOMP_HASHER_H */
