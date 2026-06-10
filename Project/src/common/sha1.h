#ifndef KVSTORE_SHA1_H
#define KVSTORE_SHA1_H

#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
 * SHA-1 (RFC 3174) — used only for the WebSocket handshake
 * (Sec-WebSocket-Accept). Not used for any security purpose.
 * ----------------------------------------------------------------------- */

#define SHA1_DIGEST_LEN 20

typedef struct {
    uint32_t state[5];
    uint64_t count;      /* Total bytes processed */
    uint8_t  buffer[64]; /* Partial block buffer */
} sha1_ctx_t;

void sha1_init(sha1_ctx_t *ctx);
void sha1_update(sha1_ctx_t *ctx, const void *data, size_t len);
void sha1_final(sha1_ctx_t *ctx, uint8_t digest[SHA1_DIGEST_LEN]);

/* One-shot convenience */
void sha1(const void *data, size_t len, uint8_t digest[SHA1_DIGEST_LEN]);

/* Base64 encode — needed alongside SHA-1 for the WebSocket handshake.
 * `out` must hold at least 4*ceil(len/3)+1 bytes. Returns strlen(out). */
size_t base64_encode(const uint8_t *data, size_t len, char *out);

#endif /* KVSTORE_SHA1_H */
