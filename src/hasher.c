#include "hasher.h"

#include "constants.h"

#include <CommonCrypto/CommonDigest.h>
#include <stdint.h>
#include <stdio.h>

/**
 * Computes the SHA-256 hash of a file as a 64-character lowercase hex string.
 */
bool hasher_file_sha256(const char *path, char output_hex[SHA256_HEX_STRING_LENGTH]) {
  if (!path || !output_hex) return false;

  FILE *file_handle = fopen(path, "rb");
  if (!file_handle) return false;

  CC_SHA256_CTX crypto_context;
  CC_SHA256_Init(&crypto_context);

  uint8_t chunk_buffer[HASHER_CHUNK_BUFFER_SIZE_BYTES];
  size_t bytes_read = 0;
  while ((bytes_read = fread(chunk_buffer, 1, sizeof(chunk_buffer), file_handle)) > 0)
    CC_SHA256_Update(&crypto_context, chunk_buffer, (CC_LONG)bytes_read);

  if (ferror(file_handle)) {
    fclose(file_handle);
    return false;
  }
  fclose(file_handle);

  uint8_t digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256_Final(digest, &crypto_context);

  /* Convert each raw byte into two hexadecimal characters */
  static const char hex_digits[] = "0123456789abcdef";
  for (size_t i = 0; i < CC_SHA256_DIGEST_LENGTH; i++) {
    const unsigned int byte = (unsigned int)digest[i];
    output_hex[i * 2] = hex_digits[(byte >> 4U) & 0x0FU];
    output_hex[(i * 2) + 1] = hex_digits[byte & 0x0FU];
  }
  output_hex[SHA256_HEX_STRING_LENGTH - 1] = '\0';
  return true;
}
