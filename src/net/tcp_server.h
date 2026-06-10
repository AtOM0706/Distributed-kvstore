#ifndef KVSTORE_TCP_SERVER_H
#define KVSTORE_TCP_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "common/buffer.h"

/* -----------------------------------------------------------------------
 * epoll-based event loop + non-blocking TCP connections.
 *
 * One net_loop_t per server process. Listeners accept connections; each
 * connection gets a read buffer (`in`) and a write buffer (`out`) for
 * partial non-blocking writes. The loop invokes `on_data` whenever new
 * bytes land in `in`, and `on_close` when the peer disconnects.
 * ----------------------------------------------------------------------- */

#define NET_MAX_EVENTS 128

typedef struct net_conn net_conn_t;
typedef struct net_loop net_loop_t;

typedef void (*net_data_cb)(net_conn_t *c);
typedef void (*net_close_cb)(net_conn_t *c);
/* Called for each newly accepted connection so the owner can set kind,
 * callbacks and user data. */
typedef void (*net_accept_cb)(net_conn_t *c, void *ud);

struct net_conn {
    int          fd;
    int          kind;       /* Owner-defined tag (client/peer/ws) */
    int          peer_id;    /* Owner use (Raft node id, 0 if n/a) */
    buffer_t     in;         /* Bytes received, not yet consumed */
    buffer_t     out;        /* Bytes queued for sending */
    bool         ws_ready;   /* WebSocket handshake completed */
    bool         closing;    /* Deferred close after flush */
    void        *user;
    void        *loop_tag_storage; /* Internal: epoll registration tag */
    net_loop_t  *loop;
    net_data_cb  on_data;
    net_close_cb on_close;
    net_conn_t  *next;       /* Intrusive list of all connections */
    net_conn_t  *prev;
};

struct net_loop {
    int         epfd;
    net_conn_t *conns;       /* All active connections */
    int         num_conns;
    bool        running;
};

/* ---- Loop ---- */
int  net_loop_init(net_loop_t *loop);
void net_loop_shutdown(net_loop_t *loop);

/* Run one iteration: wait up to timeout_ms, process I/O events.
 * Returns number of events, or -1 on fatal error. */
int net_loop_poll(net_loop_t *loop, int timeout_ms);

/* ---- Listeners ---- */

/* Listen on 0.0.0.0:port. `on_accept` configures each new connection.
 * Returns the listening fd, or -1. */
int net_loop_listen(net_loop_t *loop, int port, net_accept_cb on_accept,
                    void *ud);

/* ---- Connections ---- */

/* Register an already-connected (or connecting) non-blocking socket. */
net_conn_t *net_conn_register(net_loop_t *loop, int fd);

/* Queue bytes; flushes as much as possible immediately, the rest when
 * the socket becomes writable. Returns 0 on success. */
int net_conn_send(net_conn_t *c, const void *data, size_t len);

/* Close after the output buffer drains. */
void net_conn_close(net_conn_t *c);

/* Close immediately (frees the connection). */
void net_conn_destroy(net_conn_t *c);

/* Make any fd non-blocking */
int net_set_nonblocking(int fd);

#endif /* KVSTORE_TCP_SERVER_H */
