#include "tcp_client.h"
#include "protocol.h"
#include "common/log.h"

#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* Connection kinds (mirrors server/main.c constants — outbound only) */
#define CONN_KIND_PEER_OUT 100

static int64_t time_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void link_schedule_retry(peer_link_t *link)
{
    link->state = PEER_DISCONNECTED;
    link->conn = NULL;
    link->next_attempt_ms = time_now_ms() + link->backoff_ms;
    link->backoff_ms *= 2;
    if (link->backoff_ms > PEER_BACKOFF_MAX_MS)
        link->backoff_ms = PEER_BACKOFF_MAX_MS;
}

static void out_conn_on_close(net_conn_t *c)
{
    peer_transport_t *pt = c->user;
    peer_link_t *link = &pt->links[c->peer_id];
    if (link->conn == c) {
        LOG_DEBUG("peer", "Connection to node %d closed", c->peer_id);
        link_schedule_retry(link);
    }
}

static void out_conn_on_data(net_conn_t *c)
{
    peer_transport_t *pt = c->user;
    peer_link_t *link = &pt->links[c->peer_id];

    /* First readable event after non-blocking connect() means the
     * connection is established (we also flip on writable in tick). */
    if (link->state == PEER_CONNECTING)
        link->state = PEER_CONNECTED;

    /* Peers respond on the same socket: parse framed messages. */
    uint8_t type;
    buffer_t payload;
    buf_init(&payload, 256);
    int rc;
    while ((rc = protocol_extract(&c->in, &type, &payload)) == 1) {
        if (pt->on_message)
            pt->on_message(c->peer_id, type, &payload, pt->ud);
    }
    buf_free(&payload);
    protocol_compact(&c->in);
    if (rc < 0)
        net_conn_destroy(c); /* Triggers on_close → retry */
}

static void link_try_connect(peer_transport_t *pt, peer_link_t *link)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    net_set_nonblocking(fd);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)link->port),
    };
    if (inet_pton(AF_INET, link->host, &addr.sin_addr) != 1) {
        LOG_ERROR("peer", "Bad peer host: %s", link->host);
        close(fd);
        link_schedule_retry(link);
        return;
    }

    int rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        link_schedule_retry(link);
        return;
    }

    net_conn_t *c = net_conn_register(pt->loop, fd);
    if (!c) {
        close(fd);
        link_schedule_retry(link);
        return;
    }
    c->kind = CONN_KIND_PEER_OUT;
    c->peer_id = link->node_id;
    c->user = pt;
    c->on_data = out_conn_on_data;
    c->on_close = out_conn_on_close;

    link->conn = c;
    link->state = (rc == 0) ? PEER_CONNECTED : PEER_CONNECTING;
    LOG_DEBUG("peer", "Connecting to node %d (%s:%d)...", link->node_id,
              link->host, link->port);
}

void peer_transport_init(peer_transport_t *pt, net_loop_t *loop,
                         const cluster_config_t *config)
{
    memset(pt, 0, sizeof(*pt));
    pt->loop = loop;
    pt->self_id = config->node_id;

    for (int i = 0; i < config->num_peers; i++) {
        const peer_config_t *p = &config->peers[i];
        peer_link_t *link = &pt->links[p->node_id];
        link->node_id = p->node_id;
        snprintf(link->host, sizeof(link->host), "%s", p->host);
        link->port = p->raft_port;
        link->state = PEER_DISCONNECTED;
        link->backoff_ms = PEER_BACKOFF_MIN_MS;
        link->next_attempt_ms = 0; /* Connect ASAP */
    }
}

void peer_transport_tick(peer_transport_t *pt)
{
    int64_t now = time_now_ms();
    for (int id = 1; id <= MAX_NODES; id++) {
        peer_link_t *link = &pt->links[id];
        if (link->node_id == 0)
            continue;

        if (link->state == PEER_DISCONNECTED && now >= link->next_attempt_ms)
            link_try_connect(pt, link);

        /* Promote CONNECTING → CONNECTED once the socket is writable
         * (connect finished). Probe with a zero-byte send. */
        if (link->state == PEER_CONNECTING && link->conn) {
            int err = 0;
            socklen_t elen = sizeof(err);
            if (getsockopt(link->conn->fd, SOL_SOCKET, SO_ERROR, &err,
                           &elen) == 0) {
                if (err == 0) {
                    /* Check writability cheaply: try flushing */
                    link->state = PEER_CONNECTED;
                    link->backoff_ms = PEER_BACKOFF_MIN_MS;
                    LOG_INFO("peer", "Connected to node %d", link->node_id);
                } else if (err != EINPROGRESS) {
                    net_conn_destroy(link->conn); /* on_close → retry */
                }
            }
        }
    }
}

int peer_transport_send(peer_transport_t *pt, int node_id, uint8_t msg_type,
                        const buffer_t *payload)
{
    peer_link_t *link = &pt->links[node_id];
    if (link->state != PEER_CONNECTED || !link->conn)
        return -1; /* Dropped — Raft will retry via heartbeat */

    buffer_t framed;
    buf_init(&framed, PROTOCOL_HEADER_SIZE + payload->len);
    protocol_frame(&framed, msg_type, payload->data, (uint32_t)payload->len);
    int rc = net_conn_send(link->conn, framed.data, framed.len);
    buf_free(&framed);
    return rc;
}

bool peer_transport_is_connected(const peer_transport_t *pt, int node_id)
{
    return pt->links[node_id].state == PEER_CONNECTED;
}
