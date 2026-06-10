#ifndef KVSTORE_WEBSOCKET_H
#define KVSTORE_WEBSOCKET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "tcp_server.h"

/* -----------------------------------------------------------------------
 * Minimal WebSocket server (RFC 6455) — text frames only.
 *
 * Lifecycle on a connection accepted on the WS port:
 *   1. Browser sends an HTTP Upgrade request
 *   2. ws_handle_data() parses it, replies with 101 Switching Protocols
 *      (Sec-WebSocket-Accept = base64(SHA1(key + GUID)))
 *   3. Subsequent data is parsed as WebSocket frames; PING is answered
 *      with PONG, CLOSE closes, TEXT is delivered to the callback
 *   4. ws_send_text() sends server→client text frames (unmasked)
 * ----------------------------------------------------------------------- */

#define WS_MAX_FRAME_PAYLOAD (1024 * 1024)

typedef void (*ws_text_cb)(net_conn_t *c, const char *text, size_t len,
                           void *ud);

/* Process newly received bytes on a WebSocket connection. Call from the
 * connection's on_data callback. Performs the handshake if needed, then
 * decodes frames. `on_text` may be NULL if inbound messages are ignored.
 * Returns 0, or -1 if the connection was destroyed. */
int ws_handle_data(net_conn_t *c, ws_text_cb on_text, void *ud);

/* Send one text frame. Returns 0 on success. */
int ws_send_text(net_conn_t *c, const char *text, size_t len);

#endif /* KVSTORE_WEBSOCKET_H */
