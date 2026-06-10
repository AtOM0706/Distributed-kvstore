#ifndef KVSTORE_PROTOCOL_H
#define KVSTORE_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#include "common/buffer.h"

/* -----------------------------------------------------------------------
 * Binary message framing over TCP:
 *
 *   ┌──────────┬──────────┬──────────┐
 *   │ msg_type │ msg_len  │ payload  │
 *   │ 1 byte   │ 4B (BE)  │ variable │
 *   └──────────┴──────────┴──────────┘
 *
 * msg_len is big-endian (network byte order), matching buf_write_u32.
 *
 * TCP is a byte stream: a single read() may contain half a message or
 * three of them. protocol_extract() reassembles complete messages from
 * a per-connection accumulation buffer.
 * ----------------------------------------------------------------------- */

#define PROTOCOL_HEADER_SIZE 5
#define PROTOCOL_MAX_PAYLOAD (4 * 1024 * 1024) /* 4 MB sanity cap */

/* Append a framed message (header + payload) to `out`. */
void protocol_frame(buffer_t *out, uint8_t msg_type, const void *payload,
                    uint32_t payload_len);

/* Try to extract one complete message from `in` (starting at in->read_pos).
 *
 * Returns:
 *    1  — a message was extracted: *msg_type set, payload copied into
 *         `payload` (which is reset first), in->read_pos advanced
 *    0  — not enough bytes yet (call again after the next read)
 *   -1  — protocol violation (oversized length); close the connection
 */
int protocol_extract(buffer_t *in, uint8_t *msg_type, buffer_t *payload);

/* Discard consumed bytes from the front of `in` to keep it from growing
 * forever. Call once per event-loop iteration after extracting. */
void protocol_compact(buffer_t *in);

#endif /* KVSTORE_PROTOCOL_H */
