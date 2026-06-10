#include "common/hash.h"
#include <string.h>

/* -----------------------------------------------------------------------
 * MurmurHash3 32-bit implementation (public domain, Austin Appleby).
 * Excellent distribution and performance for hash tables and rings.
 * ----------------------------------------------------------------------- */

static inline uint32_t rotl32(uint32_t x, int r) {
    return (x << r) | (x >> (32 - r));
}

static inline uint32_t fmix32(uint32_t h) {
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

uint32_t murmurhash3_32(const void *key, size_t len, uint32_t seed) {
    const uint8_t *data = (const uint8_t *)key;
    const int nblocks = (int)(len / 4);

    uint32_t h1 = seed;

    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;

    /* Body: process 4-byte blocks */
    const uint32_t *blocks = (const uint32_t *)(data + nblocks * 4);
    for (int i = -nblocks; i; i++) {
        uint32_t k1;
        memcpy(&k1, &blocks[i], sizeof(k1));

        k1 *= c1;
        k1 = rotl32(k1, 15);
        k1 *= c2;

        h1 ^= k1;
        h1 = rotl32(h1, 13);
        h1 = h1 * 5 + 0xe6546b64;
    }

    /* Tail: process remaining bytes */
    const uint8_t *tail = data + nblocks * 4;
    uint32_t k1 = 0;

    switch (len & 3) {
    case 3: k1 ^= (uint32_t)tail[2] << 16; /* fallthrough */
    case 2: k1 ^= (uint32_t)tail[1] << 8;  /* fallthrough */
    case 1: k1 ^= (uint32_t)tail[0];
            k1 *= c1;
            k1 = rotl32(k1, 15);
            k1 *= c2;
            h1 ^= k1;
    }

    /* Finalization */
    h1 ^= (uint32_t)len;
    h1 = fmix32(h1);

    return h1;
}

uint32_t hash_string(const char *str, uint32_t seed) {
    return murmurhash3_32(str, strlen(str), seed);
}

/* -----------------------------------------------------------------------
 * CRC32 implementation (ITU-T polynomial, used for WAL checksums).
 * ----------------------------------------------------------------------- */

static uint32_t crc32_table[256];
static int crc32_table_init = 0;

static void crc32_init_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
        crc32_table[i] = crc;
    }
    crc32_table_init = 1;
}

uint32_t crc32(const void *data, size_t len) {
    if (!crc32_table_init) {
        crc32_init_table();
    }

    const uint8_t *buf = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < len; i++) {
        crc = (crc >> 8) ^ crc32_table[(crc ^ buf[i]) & 0xFF];
    }

    return crc ^ 0xFFFFFFFF;
}
