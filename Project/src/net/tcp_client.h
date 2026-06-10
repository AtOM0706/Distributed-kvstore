#ifndef KVSTORE_TCP_CLIENT_H
#define KVSTORE_TCP_CLIENT_H

#include <stdint.h>
#include <stdbool.h>

#include "common/config.h"
#include "common/buffer.h"
#include "tcp_server.h"

/* -----------------------------------------------------------------------
 * Outbound peer transport — maintains one non-blocking TCP connection to
 * each Raft peer, with automatic reconnection and exponential backoff.
 *
 * Raft RPCs are fire-and-forget at this layer: if a peer is down, the
 * message is dropped (Raft's retry-by-heartbeat handles recovery).
 * ----------------------------------------------------------------------- */

#define PEER_BACKOFF_MIN_MS 100
#define PEER_BACKOFF_MAX_MS 3000

typedef enum {
    PEER_DISCONNECTED = 0,
    PEER_CONNECTING   = 1,
    PEER_CONNECTED    = 2,
} peer_conn_state_t;

typedef struct {
    int                node_id;
    char               host[MAX_HOST_LEN];
    int                port;
    peer_conn_state_t  state;
    net_conn_t        *conn;          /* Valid when CONNECTING/CONNECTED */
    int64_t            next_attempt_ms;
    int                backoff_ms;
} peer_link_t;

typedef struct peer_transport {
    net_loop_t  *loop;
    peer_link_t  links[MAX_NODES + 1]; /* Indexed by node_id */
    int          self_id;
    /* Incoming RPC payloads are delivered through this callback
     * (shared with inbound peer connections). */
    void (*on_message)(int from_hint, uint8_t msg_type, buffer_t *payload,
                       void *ud);
    void *ud;
} peer_transport_t;

/* Initialize links from the cluster config. */
void peer_transport_init(peer_transport_t *pt, net_loop_t *loop,
                         const cluster_config_t *config);

/* Drive reconnection attempts. Call every tick. */
void peer_transport_tick(peer_transport_t *pt);

/* Send a framed message to a peer. Drops silently if not connected.
 * Returns 0 if queued. */
int peer_transport_send(peer_transport_t *pt, int node_id, uint8_t msg_type,
                        const buffer_t *payload);

bool peer_transport_is_connected(const peer_transport_t *pt, int node_id);

#endif /* KVSTORE_TCP_CLIENT_H */
