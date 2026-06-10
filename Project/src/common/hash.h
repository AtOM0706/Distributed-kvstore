#ifndef KVSTORE_HASH_H
#define KVSTORE_HASH_H

#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
 * MurmurHash3 (32-bit) — fast, well-distributed hash function.
 * Used for both the KV store hash table and consistent hashing ring.
 * ----------------------------------------------------------------------- */

/* Compute MurmurHash3 32-bit hash */
uint32_t murmurhash3_32(const void *key, size_t len, uint32_t seed);

/* Convenience wrapper for null-terminated strings */
uint32_t hash_string(const char *str, uint32_t seed);

/* CRC32 checksum for WAL integrity verification */
uint32_t crc32(const void *data, size_t len);

#endif /* KVSTORE_HASH_H */
