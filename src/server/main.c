/* -----------------------------------------------------------------------
 * kvstore-server — distributed key-value store node.
 *
 * Wires together: WAL, Raft, KV store, consistent hashing, epoll
 * networking, and the WebSocket dashboard feed.
 *
 * Startup sequence:
 *   1. Parse config
 *   2. Load snapshot (if any) into the KV store
 *   3. Open WAL, replay entries into the Raft log
 *   4. Initialize Raft (term/voted_for from raft_meta)
 *   5. Listen on client + raft + ws ports, connect to peers
 *   6. Run the epoll event loop
 * ----------------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include "common/log.h"
#include "common/config.h"
#include "common/buffer.h"
#include "wal/wal.h"
#include "wal/snapshot.h"
#include "raft/raft.h"
#include "store/kvstore.h"
#include "store/shard.h"
#include "net/tcp_server.h"
#include "net/tcp_client.h"
#include "net/websocket.h"
#include "net/protocol.h"

/* Connection kinds */
#define CONN_CLIENT  1
#define CONN_PEER_IN 2
#define CONN_WS      3

/* Client commands (MSG_CLIENT_REQUEST payload byte 0) */
#define CLIENT_CMD_SET    1
#define CLIENT_CMD_DEL    2
#define CLIENT_CMD_GET    3
#define CLIENT_CMD_STATUS 4
#define CLIENT_CMD_KEYS   5

/* Client response status codes */
#define RESP_OK         0
#define RESP_NOT_FOUND  1
#define RESP_NOT_LEADER 2
#define RESP_ERROR      3

#define MAX_PENDING 4096

typedef struct {
    uint64_t    index;     /* Raft log index we're waiting on */
    net_conn_t *conn;      /* Who to answer (NULL if disconnected) */
    int64_t     start_us;
} pending_req_t;

typedef struct {
    cluster_config_t  config;
    wal_t             wal;
    raft_log_t        raft_log;
    raft_t            raft;
    kvstore_t         kv;
    hash_ring_t       ring;
    net_loop_t        loop;
    peer_transport_t  peers;

    /* Requests waiting for Raft commit (FIFO by index) */
    pending_req_t     pending[MAX_PENDING];
    int               pending_head;
    int               pending_tail;

    /* Metrics */
    uint64_t  writes_total, reads_total;
    uint64_t  writes_last, reads_last;
    uint64_t  writes_per_sec, reads_per_sec;
    uint64_t  latency_sum_us, latency_samples;
    uint64_t  avg_latency_us;
    int64_t   started_at_ms;
    int64_t   last_metrics_ms;

    /* Recent raft events for the dashboard (ring buffer) */
    struct { char type[16]; char msg[256]; int64_t at_ms; } events[64];
    int event_head;

    volatile sig_atomic_t running;
} server_t;

static server_t S;

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int64_t now_ms(void) { return now_us() / 1000; }

/* -----------------------------------------------------------------------
 * Pending request queue
 * ----------------------------------------------------------------------- */

static int pending_count(void)
{
    return (S.pending_tail - S.pending_head + MAX_PENDING) % MAX_PENDING;
}

static int pending_push(uint64_t index, net_conn_t *conn)
{
    if (pending_count() >= MAX_PENDING - 1)
        return -1;
    S.pending[S.pending_tail] = (pending_req_t){
        .index = index, .conn = conn, .start_us = now_us(),
    };
    S.pending_tail = (S.pending_tail + 1) % MAX_PENDING;
    return 0;
}

static void pending_drop_conn(net_conn_t *conn)
{
    for (int i = S.pending_head; i != S.pending_tail;
         i = (i + 1) % MAX_PENDING) {
        if (S.pending[i].conn == conn)
            S.pending[i].conn = NULL;
    }
}

/* -----------------------------------------------------------------------
 * Client responses
 * ----------------------------------------------------------------------- */

static void send_client_response(net_conn_t *conn, uint8_t status,
                                 const char *value, uint32_t value_len)
{
    buffer_t payload, framed;
    buf_init(&payload, 16 + value_len);
    buf_write_u8(&payload, status);
    buf_write_i32(&payload, S.raft.leader_id);
    buf_write_u32(&payload, value_len);
    if (value_len > 0)
        buf_write_bytes(&payload, value, value_len);

    buf_init(&framed, payload.len + PROTOCOL_HEADER_SIZE);
    protocol_frame(&framed, MSG_CLIENT_RESPONSE, payload.data,
                   (uint32_t)payload.len);
    net_conn_send(conn, framed.data, framed.len);
    buf_free(&payload);
    buf_free(&framed);
}

/* -----------------------------------------------------------------------
 * Raft callbacks
 * ----------------------------------------------------------------------- */

static void record_event(const char *type, const char *msg)
{
    int i = S.event_head % 64;
    snprintf(S.events[i].type, sizeof(S.events[i].type), "%s", type);
    snprintf(S.events[i].msg, sizeof(S.events[i].msg), "%s", msg);
    S.events[i].at_ms = now_ms();
    S.event_head++;
}

static void raft_event(const char *type, const char *msg, void *ctx)
{
    (void)ctx;
    record_event(type, msg);
}

static void raft_send(int node_id, uint8_t msg_type, const buffer_t *payload,
                      void *ctx)
{
    (void)ctx;
    peer_transport_send(&S.peers, node_id, msg_type, payload);
}

static void check_snapshot_threshold(void);

static void raft_apply(const raft_entry_t *e, void *ctx)
{
    (void)ctx;

    if (e->cmd_type == RAFT_CMD_SET) {
        kvstore_set(&S.kv, e->key, e->key_len, e->value, e->value_len);
        S.writes_total++;
    } else if (e->cmd_type == RAFT_CMD_DEL) {
        kvstore_del(&S.kv, e->key, e->key_len);
        S.writes_total++;
    }

    /* Answer the waiting client (leader only; indices apply in order) */
    while (S.pending_head != S.pending_tail &&
           S.pending[S.pending_head].index <= e->index) {
        pending_req_t *p = &S.pending[S.pending_head];
        if (p->index == e->index && p->conn) {
            send_client_response(p->conn, RESP_OK, NULL, 0);
            int64_t lat = now_us() - p->start_us;
            S.latency_sum_us += (uint64_t)lat;
            S.latency_samples++;
        }
        S.pending_head = (S.pending_head + 1) % MAX_PENDING;
    }

    check_snapshot_threshold();
}

static int snapshot_install_load_cb(const char *key, uint32_t key_len,
                                    const char *value, uint32_t value_len,
                                    void *ctx)
{
    (void)ctx;
    kvstore_set(&S.kv, key, key_len, value, value_len);
    return 0;
}

/* Leader sent us a full snapshot: reset the store and reload. */
static int raft_snapshot_installed(const char *path, uint64_t last_index,
                                   uint64_t last_term, void *ctx)
{
    (void)ctx;
    (void)last_term;
    kvstore_clear(&S.kv);
    uint64_t idx, term;
    if (snapshot_load(path, snapshot_install_load_cb, NULL, &idx, &term) < 0)
        return -1;
    LOG_INFO("server", "State reset from leader snapshot (index %llu)",
             (unsigned long long)last_index);
    return 0;
}

/* -----------------------------------------------------------------------
 * Snapshotting (log compaction)
 * ----------------------------------------------------------------------- */

static int snapshot_write_cb(const char *key, uint32_t key_len,
                             const char *value, uint32_t value_len,
                             void *ctx)
{
    snapshot_writer_t *w = ctx;
    return snapshot_add(w, key, key_len, value, value_len);
}

static void check_snapshot_threshold(void)
{
    uint64_t applied = S.raft.last_applied;
    if (applied <= S.raft_log.base_index)
        return;
    if (applied - S.raft_log.base_index <
        (uint64_t)S.config.snapshot_threshold)
        return;

    uint64_t term = raft_log_term_at(&S.raft_log, applied);
    if (term == 0)
        return;

    LOG_INFO("server", "Snapshot threshold reached: compacting at %llu",
             (unsigned long long)applied);

    snapshot_writer_t *w = snapshot_begin(S.raft.snapshot_path, applied,
                                          term);
    if (!w)
        return;
    kvstore_foreach(&S.kv, snapshot_write_cb, w);
    if (snapshot_commit(w) == 0) {
        raft_compact_log(&S.raft, applied);
        record_event("snapshot", "Snapshot written, log compacted");
    }
}

/* -----------------------------------------------------------------------
 * Client request handling
 * ----------------------------------------------------------------------- */

static void handle_client_request(net_conn_t *conn, buffer_t *payload)
{
    if (buf_readable(payload) < 1) {
        send_client_response(conn, RESP_ERROR, NULL, 0);
        return;
    }
    uint8_t cmd = buf_read_u8(payload);

    char key[4096];
    size_t key_len = 0;
    if (cmd == CLIENT_CMD_SET || cmd == CLIENT_CMD_DEL ||
        cmd == CLIENT_CMD_GET) {
        if (buf_readable(payload) < 4) {
            send_client_response(conn, RESP_ERROR, NULL, 0);
            return;
        }
        key_len = buf_read_str(payload, key, sizeof(key));
        if (key_len == 0 || key_len > (size_t)S.config.max_key_size) {
            send_client_response(conn, RESP_ERROR, NULL, 0);
            return;
        }
    }

    switch (cmd) {
    case CLIENT_CMD_GET: {
        if (!raft_is_leader(&S.raft)) {
            send_client_response(conn, RESP_NOT_LEADER, NULL, 0);
            return;
        }
        const char *value;
        uint32_t value_len;
        S.reads_total++;
        if (kvstore_get(&S.kv, key, (uint32_t)key_len, &value, &value_len))
            send_client_response(conn, RESP_OK, value, value_len);
        else
            send_client_response(conn, RESP_NOT_FOUND, NULL, 0);
        return;
    }

    case CLIENT_CMD_SET:
    case CLIENT_CMD_DEL: {
        if (!raft_is_leader(&S.raft)) {
            send_client_response(conn, RESP_NOT_LEADER, NULL, 0);
            return;
        }

        char *value = NULL;
        uint32_t value_len = 0;
        if (cmd == CLIENT_CMD_SET) {
            if (buf_readable(payload) < 4) {
                send_client_response(conn, RESP_ERROR, NULL, 0);
                return;
            }
            value_len = buf_read_u32(payload);
            if (value_len > (uint32_t)S.config.max_value_size ||
                buf_readable(payload) < value_len) {
                send_client_response(conn, RESP_ERROR, NULL, 0);
                return;
            }
            value = malloc(value_len + 1);
            if (!value) {
                send_client_response(conn, RESP_ERROR, NULL, 0);
                return;
            }
            buf_read_bytes(payload, value, value_len);
            value[value_len] = '\0';
        }

        uint8_t raft_cmd = (cmd == CLIENT_CMD_SET) ? RAFT_CMD_SET
                                                   : RAFT_CMD_DEL;
        uint64_t index = raft_submit(&S.raft, raft_cmd, key,
                                     (uint32_t)key_len, value, value_len);
        free(value);

        if (index == 0 || pending_push(index, conn) < 0) {
            send_client_response(conn, RESP_ERROR, NULL, 0);
            return;
        }
        /* Response is sent when the entry commits (raft_apply) */
        return;
    }

    case CLIENT_CMD_STATUS: {
        char status[1024];
        int len = snprintf(status, sizeof(status),
            "node=%d role=%s term=%llu commit=%llu applied=%llu keys=%zu "
            "leader=%d peers=%d wal_entries=%llu",
            S.config.node_id, raft_role_name(S.raft.role),
            (unsigned long long)S.raft.current_term,
            (unsigned long long)S.raft.commit_index,
            (unsigned long long)S.raft.last_applied,
            kvstore_count(&S.kv), S.raft.leader_id, S.config.num_peers,
            (unsigned long long)S.wal.entry_count);
        send_client_response(conn, RESP_OK, status, (uint32_t)len);
        return;
    }

    case CLIENT_CMD_KEYS: {
        char buf[32];
        int len = snprintf(buf, sizeof(buf), "%zu", kvstore_count(&S.kv));
        send_client_response(conn, RESP_OK, buf, (uint32_t)len);
        return;
    }

    default:
        send_client_response(conn, RESP_ERROR, NULL, 0);
    }
}

/* -----------------------------------------------------------------------
 * Connection data handlers
 * ----------------------------------------------------------------------- */

static void client_on_data(net_conn_t *c)
{
    uint8_t type;
    buffer_t payload;
    buf_init(&payload, 256);
    int rc;
    while ((rc = protocol_extract(&c->in, &type, &payload)) == 1) {
        if (type == MSG_CLIENT_REQUEST)
            handle_client_request(c, &payload);
    }
    buf_free(&payload);
    protocol_compact(&c->in);
    if (rc < 0)
        net_conn_destroy(c);
}

static void client_on_close(net_conn_t *c)
{
    pending_drop_conn(c);
}

static void peer_on_data(net_conn_t *c)
{
    uint8_t type;
    buffer_t payload;
    buf_init(&payload, 256);
    int rc;
    while ((rc = protocol_extract(&c->in, &type, &payload)) == 1)
        raft_handle_message(&S.raft, type, &payload);
    buf_free(&payload);
    protocol_compact(&c->in);
    if (rc < 0)
        net_conn_destroy(c);
}

/* Messages arriving on our *outbound* connections (responses) */
static void peer_out_message(int from_hint, uint8_t msg_type,
                             buffer_t *payload, void *ud)
{
    (void)from_hint;
    (void)ud;
    raft_handle_message(&S.raft, msg_type, payload);
}

static void ws_on_data(net_conn_t *c)
{
    ws_handle_data(c, NULL, NULL); /* Inbound WS messages are ignored */
}

static void client_accept(net_conn_t *c, void *ud)
{
    (void)ud;
    c->kind = CONN_CLIENT;
    c->on_data = client_on_data;
    c->on_close = client_on_close;
}

static void peer_accept(net_conn_t *c, void *ud)
{
    (void)ud;
    c->kind = CONN_PEER_IN;
    c->on_data = peer_on_data;
}

static void ws_accept(net_conn_t *c, void *ud)
{
    (void)ud;
    c->kind = CONN_WS;
    c->on_data = ws_on_data;
}

/* -----------------------------------------------------------------------
 * Dashboard JSON broadcast
 * ----------------------------------------------------------------------- */

static void broadcast_status(void)
{
    /* Build the JSON message */
    static char json[65536];
    size_t off = 0;

    #define JADD(...) do { \
        int _n = snprintf(json + off, sizeof(json) - off, __VA_ARGS__); \
        if (_n > 0) off += (size_t)_n < sizeof(json) - off ? (size_t)_n \
                                       : sizeof(json) - off - 1; \
    } while (0)

    JADD("{\"type\":\"status\",\"node_id\":%d,\"role\":\"%s\","
         "\"term\":%llu,\"commit_index\":%llu,\"leader_id\":%d,",
         S.config.node_id, raft_role_name(S.raft.role),
         (unsigned long long)S.raft.current_term,
         (unsigned long long)S.raft.commit_index, S.raft.leader_id);

    /* Peers */
    JADD("\"peers\":[");
    for (int i = 0; i < S.config.num_peers; i++) {
        int pid = S.config.peers[i].node_id;
        JADD("%s{\"id\":%d,\"match_index\":%llu,\"connected\":%s}",
             i ? "," : "", pid,
             (unsigned long long)S.raft.peers[pid].match_index,
             peer_transport_is_connected(&S.peers, pid) ? "true" : "false");
    }
    JADD("],");

    /* Metrics */
    JADD("\"metrics\":{\"writes_per_sec\":%llu,\"reads_per_sec\":%llu,"
         "\"avg_latency_us\":%llu,\"total_keys\":%zu,"
         "\"wal_size_bytes\":%llu,\"uptime_sec\":%lld},",
         (unsigned long long)S.writes_per_sec,
         (unsigned long long)S.reads_per_sec,
         (unsigned long long)S.avg_latency_us, kvstore_count(&S.kv),
         (unsigned long long)S.wal.file_size,
         (long long)((now_ms() - S.started_at_ms) / 1000));

    /* Hash ring (sampled: every 3rd vnode keeps the payload light) */
    JADD("\"hash_ring\":{\"vnodes\":[");
    int emitted = 0;
    for (int i = 0; i < S.ring.size; i += 3) {
        JADD("%s{\"hash\":%u,\"node_id\":%d}", emitted ? "," : "",
             S.ring.ring[i].hash, S.ring.ring[i].node_id);
        emitted++;
    }
    JADD("]},");

    /* Recent events (last 10) */
    JADD("\"events\":[");
    int start = S.event_head > 10 ? S.event_head - 10 : 0;
    for (int i = start; i < S.event_head; i++) {
        const char *m = S.events[i % 64].msg;
        /* Escape quotes crudely (events contain none by construction) */
        JADD("%s{\"type\":\"%s\",\"msg\":\"%s\",\"at\":%lld}",
             i > start ? "," : "", S.events[i % 64].type, m,
             (long long)S.events[i % 64].at_ms);
    }
    JADD("]}");
    #undef JADD

    for (net_conn_t *c = S.loop.conns; c; ) {
        net_conn_t *next = c->next; /* c may be destroyed while sending */
        if (c->kind == CONN_WS && c->ws_ready)
            ws_send_text(c, json, off);
        c = next;
    }
}

static void update_metrics(void)
{
    S.writes_per_sec = S.writes_total - S.writes_last;
    S.reads_per_sec = S.reads_total - S.reads_last;
    S.writes_last = S.writes_total;
    S.reads_last = S.reads_total;
    if (S.latency_samples > 0) {
        S.avg_latency_us = S.latency_sum_us / S.latency_samples;
        S.latency_sum_us = 0;
        S.latency_samples = 0;
    }
}

/* -----------------------------------------------------------------------
 * Startup / recovery
 * ----------------------------------------------------------------------- */

static int kv_load_cb(const char *key, uint32_t key_len, const char *value,
                      uint32_t value_len, void *ctx)
{
    (void)ctx;
    return kvstore_set(&S.kv, key, key_len, value, value_len);
}

static void on_signal(int sig)
{
    (void)sig;
    S.running = 0;
}

int main(int argc, char **argv)
{
    if (config_parse(&S.config, argc, argv) < 0)
        return 1;

    log_set_level(getenv("KV_LOG_DEBUG") ? LOG_LEVEL_DEBUG : LOG_LEVEL_INFO);
    LOG_INFO("server", "=== kvstore-server node %d starting ===",
             S.config.node_id);
    config_print(&S.config);

    if (mkdir(S.config.data_dir, 0755) < 0 && errno != EEXIST) {
        LOG_FATAL("server", "Cannot create data dir %s: %s",
                  S.config.data_dir, strerror(errno));
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    srand((unsigned)(time(NULL) ^ (S.config.node_id * 7919)));

    if (kvstore_init(&S.kv) < 0)
        return 1;

    /* 1. Load snapshot into the KV store */
    char snap_path[512];
    snprintf(snap_path, sizeof(snap_path), "%s/snapshot.db",
             S.config.data_dir);
    uint64_t snap_index = 0, snap_term = 0;
    if (snapshot_load(snap_path, kv_load_cb, NULL, &snap_index,
                      &snap_term) < 0) {
        LOG_WARN("server", "Snapshot corrupt — starting from WAL only");
        kvstore_clear(&S.kv);
        snap_index = snap_term = 0;
    }

    /* 2. Open the WAL and rebuild the Raft log */
    char wal_path[512];
    snprintf(wal_path, sizeof(wal_path), "%s/wal.log", S.config.data_dir);
    if (wal_open(&S.wal, wal_path) < 0)
        return 1;

    raft_log_init(&S.raft_log, &S.wal);
    S.raft_log.base_index = snap_index;
    S.raft_log.base_term = snap_term;
    if (raft_log_restore(&S.raft_log) < 0)
        return 1;

    /* 3. Raft */
    if (raft_init(&S.raft, &S.config, &S.raft_log) < 0)
        return 1;
    raft_restore_applied(&S.raft, snap_index);
    raft_set_send_cb(&S.raft, raft_send, NULL);
    raft_set_apply_cb(&S.raft, raft_apply, NULL);
    raft_set_event_cb(&S.raft, raft_event, NULL);
    raft_set_snapshot_cb(&S.raft, raft_snapshot_installed, NULL);

    /* Re-apply WAL entries up to the previous commit point is not needed:
     * entries become visible once the new leader confirms commit. The KV
     * store currently reflects exactly the snapshot state. */

    /* 4. Hash ring (all cluster members) */
    hash_ring_init(&S.ring);
    hash_ring_add_node(&S.ring, S.config.node_id);
    for (int i = 0; i < S.config.num_peers; i++)
        hash_ring_add_node(&S.ring, S.config.peers[i].node_id);

    /* 5. Networking */
    if (net_loop_init(&S.loop) < 0)
        return 1;
    if (net_loop_listen(&S.loop, S.config.client_port, client_accept,
                        NULL) < 0 ||
        net_loop_listen(&S.loop, S.config.raft_port, peer_accept, NULL) < 0 ||
        net_loop_listen(&S.loop, S.config.ws_port, ws_accept, NULL) < 0)
        return 1;

    peer_transport_init(&S.peers, &S.loop, &S.config);
    S.peers.on_message = peer_out_message;

    S.running = 1;
    S.started_at_ms = now_ms();
    S.last_metrics_ms = now_ms();

    LOG_INFO("server", "Node %d ready: client=%d raft=%d ws=%d",
             S.config.node_id, S.config.client_port, S.config.raft_port,
             S.config.ws_port);

    /* 6. Event loop */
    while (S.running) {
        net_loop_poll(&S.loop, 10);
        peer_transport_tick(&S.peers);
        raft_tick(&S.raft);

        int64_t now = now_ms();
        if (now - S.last_metrics_ms >= 1000) {
            S.last_metrics_ms = now;
            update_metrics();
            broadcast_status();
        }
    }

    LOG_INFO("server", "Shutting down");
    net_loop_shutdown(&S.loop);
    wal_close(&S.wal);
    raft_log_free(&S.raft_log);
    kvstore_free(&S.kv);
    return 0;
}
