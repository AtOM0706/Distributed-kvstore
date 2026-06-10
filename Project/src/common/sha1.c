#include "sha1.h"
#include <string.h>

/* -----------------------------------------------------------------------
 * SHA-1 implementation (RFC 3174). Public-domain style implementation.
 * Used exclusively for the WebSocket upgrade handshake.
 * ----------------------------------------------------------------------- */

#define ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_transform(uint32_t state[5], const uint8_t block[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; i++)
        w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = state[0], b = state[1], c = state[2];
    uint32_t d = state[3], e = state[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t tmp = ROL32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL32(b, 30);
        b = a;
        a = tmp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void sha1_init(sha1_ctx_t *ctx)
{
    ctx->state[0] = 0x67452301;
    ctx->state[1] = 0xEFCDAB89;
    ctx->state[2] = 0x98BADCFE;
    ctx->state[3] = 0x10325476;
    ctx->state[4] = 0xC3D2E1F0;
    ctx->count = 0;
}

void sha1_update(sha1_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t buffered = (size_t)(ctx->count % 64);
    ctx->count += len;

    /* Fill partial block first */
    if (buffered > 0) {
        size_t need = 64 - buffered;
        if (len < need) {
            memcpy(ctx->buffer + buffered, p, len);
            return;
        }
        memcpy(ctx->buffer + buffered, p, need);
        sha1_transform(ctx->state, ctx->buffer);
        p += need;
        len -= need;
    }

    /* Process full blocks directly */
    while (len >= 64) {
        sha1_transform(ctx->state, p);
        p += 64;
        len -= 64;
    }

    /* Buffer the remainder */
    if (len > 0)
        memcpy(ctx->buffer, p, len);
}

void sha1_final(sha1_ctx_t *ctx, uint8_t digest[SHA1_DIGEST_LEN])
{
    uint64_t bit_count = ctx->count * 8;
    size_t buffered = (size_t)(ctx->count % 64);

    /* Append 0x80, then zero-pad to 56 mod 64, then 64-bit length */
    uint8_t pad[72] = { 0x80 };
    size_t pad_len = (buffered < 56) ? (56 - buffered) : (120 - buffered);

    sha1_update(ctx, pad, pad_len);

    uint8_t len_be[8];
    for (int i = 0; i < 8; i++)
        len_be[i] = (uint8_t)(bit_count >> (56 - i * 8));
    sha1_update(ctx, len_be, 8);

    for (int i = 0; i < 5; i++) {
        digest[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

void sha1(const void *data, size_t len, uint8_t digest[SHA1_DIGEST_LEN])
{
    sha1_ctx_t ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, digest);
}

/* -----------------------------------------------------------------------
 * Base64 encoding (RFC 4648)
 * ----------------------------------------------------------------------- */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t base64_encode(const uint8_t *data, size_t len, char *out)
{
    size_t o = 0;
    size_t i;
    for (i = 0; i + 2 < len; i += 3) {
        uint32_t n = ((uint32_t)data[i] << 16) |
                     ((uint32_t)data[i + 1] << 8) |
                     ((uint32_t)data[i + 2]);
        out[o++] = b64_table[(n >> 18) & 63];
        out[o++] = b64_table[(n >> 12) & 63];
        out[o++] = b64_table[(n >> 6) & 63];
        out[o++] = b64_table[n & 63];
    }
    if (i < len) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < len)
            n |= (uint32_t)data[i + 1] << 8;
        out[o++] = b64_table[(n >> 18) & 63];
        out[o++] = b64_table[(n >> 12) & 63];
        out[o++] = (i + 1 < len) ? b64_table[(n >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}
