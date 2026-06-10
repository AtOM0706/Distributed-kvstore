#ifndef KVSTORE_BUFFER_H
#define KVSTORE_BUFFER_H

#include <stddef.h>
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Dynamic byte buffer with automatic growth.
 * Used for network message serialization/deserialization.
 *
 * Usage:
 *   buffer_t buf;
 *   buf_init(&buf, 256);
 *   buf_write_u32(&buf, 42);
 *   buf_write_str(&buf, "hello");
 *   buf_write_bytes(&buf, data, len);
 *   // ... send buf.data[0..buf.len] over the wire ...
 *   buf_free(&buf);
 * ----------------------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t   len;      /* Current number of bytes written */
    size_t   cap;      /* Allocated capacity */
    size_t   read_pos; /* Current read cursor for deserialization */
} buffer_t;

/* Initialize a buffer with the given initial capacity */
void buf_init(buffer_t *buf, size_t initial_cap);

/* Free the buffer's memory */
void buf_free(buffer_t *buf);

/* Reset length and read position to 0 (does not free memory) */
void buf_reset(buffer_t *buf);

/* Ensure at least `needed` bytes of free space */
void buf_ensure(buffer_t *buf, size_t needed);

/* ---- Write operations (append to end) ---- */
void buf_write_u8(buffer_t *buf, uint8_t val);
void buf_write_u16(buffer_t *buf, uint16_t val);
void buf_write_u32(buffer_t *buf, uint32_t val);
void buf_write_u64(buffer_t *buf, uint64_t val);
void buf_write_i32(buffer_t *buf, int32_t val);
void buf_write_i64(buffer_t *buf, int64_t val);
void buf_write_bytes(buffer_t *buf, const void *data, size_t len);
void buf_write_str(buffer_t *buf, const char *str);  /* length-prefixed string */

/* ---- Read operations (advance read cursor) ---- */
uint8_t  buf_read_u8(buffer_t *buf);
uint16_t buf_read_u16(buffer_t *buf);
uint32_t buf_read_u32(buffer_t *buf);
uint64_t buf_read_u64(buffer_t *buf);
int32_t  buf_read_i32(buffer_t *buf);
int64_t  buf_read_i64(buffer_t *buf);
size_t   buf_read_bytes(buffer_t *buf, void *out, size_t len);
size_t   buf_read_str(buffer_t *buf, char *out, size_t max_len);

/* How many bytes remain to be read */
size_t buf_readable(const buffer_t *buf);

#endif /* KVSTORE_BUFFER_H */
