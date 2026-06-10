#include "websocket.h"
#include "common/log.h"
#include "common/sha1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* Opcodes */
#define WS_OP_CONT  0x0
#define WS_OP_TEXT  0x1
#define WS_OP_BIN   0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING  0x9
#define WS_OP_PONG  0xA

/* -----------------------------------------------------------------------
 * Handshake
 * ----------------------------------------------------------------------- */

/* Case-insensitive header lookup inside the raw request. Returns a
 * pointer to the value (not NUL-terminated) and its length. */
static const char *find_header(const char *req, size_t req_len,
                               const char *name, size_t *out_len)
{
    size_t name_len = strlen(name);
    const char *p = req;
    const char *end = req + req_len;

    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        if (!line_end)
            break;
        size_t line_len = (size_t)(line_end - p);
        if (line_len > name_len + 1 &&
            strncasecmp(p, name, name_len) == 0 && p[name_len] == ':') {
            const char *v = p + name_len + 1;
            while (v < line_end && (*v == ' ' || *v == '\t'))
                v++;
            const char *ve = line_end;
            while (ve > v && (ve[-1] == '\r' || ve[-1] == ' '))
                ve--;
            *out_len = (size_t)(ve - v);
            return v;
        }
        p = line_end + 1;
    }
    return NULL;
}

static int ws_do_handshake(net_conn_t *c)
{
    /* Wait until we have the full request head */
    const char *req = (const char *)c->in.data;
    size_t len = c->in.len;
    const char *head_end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (memcmp(req + i, "\r\n\r\n", 4) == 0) {
            head_end = req + i + 4;
            break;
        }
    }
    if (!head_end)
        return 0; /* Need more bytes */

    size_t key_len;
    const char *key = find_header(req, len, "Sec-WebSocket-Key", &key_len);
    if (!key || key_len == 0 || key_len > 64) {
        LOG_WARN("ws", "Bad upgrade request (no Sec-WebSocket-Key)");
        net_conn_destroy(c);
        return -1;
    }

    /* accept = base64(SHA1(key + GUID)) */
    char concat[128];
    int concat_len = snprintf(concat, sizeof(concat), "%.*s%s",
                              (int)key_len, key, WS_GUID);
    uint8_t digest[SHA1_DIGEST_LEN];
    sha1(concat, (size_t)concat_len, digest);
    char accept_b64[64];
    base64_encode(digest, SHA1_DIGEST_LEN, accept_b64);

    char resp[256];
    int resp_len = snprintf(resp, sizeof(resp),
                            "HTTP/1.1 101 Switching Protocols\r\n"
                            "Upgrade: websocket\r\n"
                            "Connection: Upgrade\r\n"
                            "Sec-WebSocket-Accept: %s\r\n\r\n",
                            accept_b64);

    /* Consume the request and reply */
    size_t consumed = (size_t)(head_end - req);
    c->in.read_pos = consumed;
    {
        size_t remaining = c->in.len - c->in.read_pos;
        memmove(c->in.data, c->in.data + c->in.read_pos, remaining);
        c->in.len = remaining;
        c->in.read_pos = 0;
    }

    if (net_conn_send(c, resp, (size_t)resp_len) < 0)
        return -1;
    c->ws_ready = true;
    LOG_DEBUG("ws", "Handshake complete (fd %d)", c->fd);
    return 1;
}

/* -----------------------------------------------------------------------
 * Frames
 * ----------------------------------------------------------------------- */

int ws_send_text(net_conn_t *c, const char *text, size_t len)
{
    if (!c->ws_ready)
        return -1;

    uint8_t hdr[10];
    size_t hdr_len;
    hdr[0] = 0x80 | WS_OP_TEXT; /* FIN + text */

    if (len < 126) {
        hdr[1] = (uint8_t)len;
        hdr_len = 2;
    } else if (len <= 0xFFFF) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)len;
        hdr_len = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++)
            hdr[2 + i] = (uint8_t)(len >> (56 - 8 * i));
        hdr_len = 10;
    }

    if (net_conn_send(c, hdr, hdr_len) < 0)
        return -1;
    return net_conn_send(c, text, len);
}

static int ws_send_control(net_conn_t *c, uint8_t opcode,
                           const uint8_t *payload, size_t len)
{
    uint8_t hdr[2] = { (uint8_t)(0x80 | opcode), (uint8_t)len };
    if (net_conn_send(c, hdr, 2) < 0)
        return -1;
    if (len > 0)
        return net_conn_send(c, payload, len);
    return 0;
}

/* Try to decode one frame from c->in. Returns:
 *   1 = frame consumed, 0 = need more bytes, -1 = connection destroyed */
static int ws_decode_frame(net_conn_t *c, ws_text_cb on_text, void *ud)
{
    const uint8_t *p = c->in.data + c->in.read_pos;
    size_t avail = c->in.len - c->in.read_pos;

    if (avail < 2)
        return 0;

    uint8_t opcode = p[0] & 0x0F;
    bool masked = (p[1] & 0x80) != 0;
    uint64_t plen = p[1] & 0x7F;
    size_t pos = 2;

    if (plen == 126) {
        if (avail < 4)
            return 0;
        plen = ((uint64_t)p[2] << 8) | p[3];
        pos = 4;
    } else if (plen == 127) {
        if (avail < 10)
            return 0;
        plen = 0;
        for (int i = 0; i < 8; i++)
            plen = (plen << 8) | p[2 + i];
        pos = 10;
    }

    if (plen > WS_MAX_FRAME_PAYLOAD) {
        net_conn_destroy(c);
        return -1;
    }

    uint8_t mask[4] = {0};
    if (masked) {
        if (avail < pos + 4)
            return 0;
        memcpy(mask, p + pos, 4);
        pos += 4;
    }

    if (avail < pos + plen)
        return 0;

    /* Unmask in place */
    uint8_t *data = (uint8_t *)(p + pos);
    if (masked)
        for (uint64_t i = 0; i < plen; i++)
            data[i] ^= mask[i % 4];

    c->in.read_pos += pos + plen;

    switch (opcode) {
    case WS_OP_TEXT:
    case WS_OP_BIN:
        if (on_text)
            on_text(c, (const char *)data, (size_t)plen, ud);
        return 1;
    case WS_OP_PING:
        if (ws_send_control(c, WS_OP_PONG, data, (size_t)plen) < 0)
            return -1;
        return 1;
    case WS_OP_CLOSE:
        ws_send_control(c, WS_OP_CLOSE, NULL, 0);
        net_conn_close(c);
        return -1;
    case WS_OP_PONG:
    case WS_OP_CONT:
    default:
        return 1; /* Ignore */
    }
}

int ws_handle_data(net_conn_t *c, ws_text_cb on_text, void *ud)
{
    if (!c->ws_ready) {
        int rc = ws_do_handshake(c);
        if (rc <= 0)
            return rc; /* 0 = need more, -1 = destroyed */
    }

    int rc;
    while ((rc = ws_decode_frame(c, on_text, ud)) == 1)
        ;
    if (rc == -1)
        return -1;

    /* Compact the input buffer */
    size_t remaining = c->in.len - c->in.read_pos;
    if (c->in.read_pos > 0 && remaining > 0)
        memmove(c->in.data, c->in.data + c->in.read_pos, remaining);
    c->in.len = remaining;
    c->in.read_pos = 0;
    return 0;
}
