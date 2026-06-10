#include "protocol.h"

#include <string.h>

void protocol_frame(buffer_t *out, uint8_t msg_type, const void *payload,
                    uint32_t payload_len)
{
    buf_write_u8(out, msg_type);
    buf_write_u32(out, payload_len);
    if (payload_len > 0)
        buf_write_bytes(out, payload, payload_len);
}

int protocol_extract(buffer_t *in, uint8_t *msg_type, buffer_t *payload)
{
    if (buf_readable(in) < PROTOCOL_HEADER_SIZE)
        return 0;

    /* Peek the header without consuming (length is network byte order,
     * matching buf_write_u32) */
    const uint8_t *p = in->data + in->read_pos;
    uint8_t type = p[0];
    uint32_t len = ((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16) |
                   ((uint32_t)p[3] << 8) | (uint32_t)p[4];

    if (len > PROTOCOL_MAX_PAYLOAD)
        return -1;
    if (buf_readable(in) < PROTOCOL_HEADER_SIZE + (size_t)len)
        return 0;

    in->read_pos += PROTOCOL_HEADER_SIZE;
    buf_reset(payload);
    if (len > 0) {
        buf_write_bytes(payload, in->data + in->read_pos, len);
        in->read_pos += len;
    }
    *msg_type = type;
    return 1;
}

void protocol_compact(buffer_t *in)
{
    if (in->read_pos == 0)
        return;
    size_t remaining = in->len - in->read_pos;
    if (remaining > 0)
        memmove(in->data, in->data + in->read_pos, remaining);
    in->len = remaining;
    in->read_pos = 0;
}
