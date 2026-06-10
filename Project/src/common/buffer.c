#include "common/buffer.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

/* -----------------------------------------------------------------------
 * Dynamic byte buffer implementation.
 * All multi-byte integers are stored in network byte order (big-endian).
 * ----------------------------------------------------------------------- */

void buf_init(buffer_t *buf, size_t initial_cap) {
    if (initial_cap < 64) initial_cap = 64;
    buf->data = (uint8_t *)malloc(initial_cap);
    if (!buf->data) {
        LOG_FATAL("buffer", "Failed to allocate %zu bytes", initial_cap);
    }
    buf->len = 0;
    buf->cap = initial_cap;
    buf->read_pos = 0;
}

void buf_free(buffer_t *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
    buf->read_pos = 0;
}

void buf_reset(buffer_t *buf) {
    buf->len = 0;
    buf->read_pos = 0;
}

void buf_ensure(buffer_t *buf, size_t needed) {
    if (buf->len + needed <= buf->cap)
        return;
    size_t new_cap = buf->cap;
    while (new_cap < buf->len + needed) {
        new_cap *= 2;
    }
    uint8_t *new_data = (uint8_t *)realloc(buf->data, new_cap);
    if (!new_data) {
        LOG_FATAL("buffer", "Failed to realloc to %zu bytes", new_cap);
    }
    buf->data = new_data;
    buf->cap = new_cap;
}

/* ---- Write operations ---- */

void buf_write_u8(buffer_t *buf, uint8_t val) {
    buf_ensure(buf, 1);
    buf->data[buf->len++] = val;
}

void buf_write_u16(buffer_t *buf, uint16_t val) {
    buf_ensure(buf, 2);
    uint16_t nval = htons(val);
    memcpy(buf->data + buf->len, &nval, 2);
    buf->len += 2;
}

void buf_write_u32(buffer_t *buf, uint32_t val) {
    buf_ensure(buf, 4);
    uint32_t nval = htonl(val);
    memcpy(buf->data + buf->len, &nval, 4);
    buf->len += 4;
}

void buf_write_u64(buffer_t *buf, uint64_t val) {
    buf_ensure(buf, 8);
    uint64_t nval = htobe64(val);
    memcpy(buf->data + buf->len, &nval, 8);
    buf->len += 8;
}

void buf_write_i32(buffer_t *buf, int32_t val) {
    buf_write_u32(buf, (uint32_t)val);
}

void buf_write_i64(buffer_t *buf, int64_t val) {
    buf_write_u64(buf, (uint64_t)val);
}

void buf_write_bytes(buffer_t *buf, const void *data, size_t len) {
    buf_ensure(buf, len);
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
}

void buf_write_str(buffer_t *buf, const char *str) {
    uint32_t slen = (uint32_t)strlen(str);
    buf_write_u32(buf, slen);
    buf_write_bytes(buf, str, slen);
}

/* ---- Read operations ---- */

static int check_readable(const buffer_t *buf, size_t needed) {
    if (buf->read_pos + needed > buf->len) {
        LOG_ERROR("buffer", "Buffer underflow: need %zu bytes, have %zu",
                  needed, buf->len - buf->read_pos);
        return 0;
    }
    return 1;
}

uint8_t buf_read_u8(buffer_t *buf) {
    if (!check_readable(buf, 1)) return 0;
    return buf->data[buf->read_pos++];
}

uint16_t buf_read_u16(buffer_t *buf) {
    if (!check_readable(buf, 2)) return 0;
    uint16_t nval;
    memcpy(&nval, buf->data + buf->read_pos, 2);
    buf->read_pos += 2;
    return ntohs(nval);
}

uint32_t buf_read_u32(buffer_t *buf) {
    if (!check_readable(buf, 4)) return 0;
    uint32_t nval;
    memcpy(&nval, buf->data + buf->read_pos, 4);
    buf->read_pos += 4;
    return ntohl(nval);
}

uint64_t buf_read_u64(buffer_t *buf) {
    if (!check_readable(buf, 8)) return 0;
    uint64_t nval;
    memcpy(&nval, buf->data + buf->read_pos, 8);
    buf->read_pos += 8;
    return be64toh(nval);
}

int32_t buf_read_i32(buffer_t *buf) {
    return (int32_t)buf_read_u32(buf);
}

int64_t buf_read_i64(buffer_t *buf) {
    return (int64_t)buf_read_u64(buf);
}

size_t buf_read_bytes(buffer_t *buf, void *out, size_t len) {
    if (!check_readable(buf, len)) return 0;
    memcpy(out, buf->data + buf->read_pos, len);
    buf->read_pos += len;
    return len;
}

size_t buf_read_str(buffer_t *buf, char *out, size_t max_len) {
    uint32_t slen = buf_read_u32(buf);
    if (slen == 0) {
        if (max_len > 0) out[0] = '\0';
        return 0;
    }
    if (!check_readable(buf, slen)) return 0;
    size_t copy_len = (slen < max_len - 1) ? slen : max_len - 1;
    memcpy(out, buf->data + buf->read_pos, copy_len);
    out[copy_len] = '\0';
    buf->read_pos += slen;
    return copy_len;
}

size_t buf_readable(const buffer_t *buf) {
    return buf->len - buf->read_pos;
}
