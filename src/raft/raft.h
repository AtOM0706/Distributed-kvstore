#ifndef KVSTORE_RAFT_H
#define KVSTORE_RAFT_H

#include <stdint.h>
#include <stdbool.h>

#include "common/config.h"
#include "common/buffer.h"
#include "raft_log.h"
#include "raft_rpc.h"
#include "raft_timer.h"

/* -----------------------------------------------------------------------
 * Raft consensus engine (single-threaded, event-loop driven).
 *
 * The engine is transport-agnostic: it emits outgoing messages through
 * `send_cb` and the server feeds incoming messages to the
 * raft_handle_*() functions. Committed entries are delivered through
 * `apply_cb`.
 *
 * Threading model: ALL functions must be called from the event-loop
 * thread. No internal locking.
 * ----------------------------------------------------------------------- */

typedef enum {
    RAFT_FOLLOWER  = 0,
    RAFT_CANDIDATE = 1,
    RAFT_LEADER    = 2,
} raft_role_t;

const char *raft_role_name(raft_role_t role);

/* Outgoing message: send `payload` (already serialized) of `msg_type`
 * to peer `node_id`. */
typedef void (*raft_send_cb)(int node_id, uint8_t msg_type,
                             const buffer_t *payload, void *ctx);

/* A log entry has been committed — apply it to the state machine. */
typedef void (*raft_apply_cb)(const raft_entry_t *entry, void *ctx);

/* Human-readable cluster event (election won, vote cast, ...) for the
 * dashboard's live log viewer. */
typedef void (*raft_event_cb)(const char *event_type, const char *message,
                              void *ctx);

/* A snapshot was received from the leader and installed at `path`.
 * The server must reset its KV store and reload from this file.
 * Return 0 on success. */
typedef int (*raft_snapshot_installed_cb)(const char *path,
                                          uint64_t last_index,
                                          uint64_t last_term, void *ctx);

/* Per-peer replication state (leader only) */
typedef struct {
    uint64_t next_index;     /* Next log index to send */
    uint64_t match_index;    /* Highest index known replicated */
    bool     snapshot_inflight;
    uint64_t snapshot_offset;
    int64_t  last_ae_sent_ms; /* For RPC pacing */
} raft_peer_t;

typedef struct {
    /* --- Persistent state (durable via meta file + WAL) --- */
    uint64_t current_term;
    int      voted_for;       /* -1 = none */

    /* --- Volatile state --- */
    raft_role_t role;
    int         leader_id;    /* -1 = unknown */
    uint64_t    commit_index;
    uint64_t    last_applied;

    /* --- Leader state --- */
    raft_peer_t peers[MAX_NODES + 1]; /* Indexed by node_id (1-based) */

    /* --- Election bookkeeping --- */
    int votes_received;

    /* --- Batching --- */
    bool replication_pending; /* New entries await broadcast (next tick) */

    /* --- Components --- */
    raft_log_t       *log;
    cluster_config_t *config;
    raft_timer_t      timer;
    char              meta_path[512];
    char              snapshot_path[512];

    /* --- Callbacks --- */
    raft_send_cb               send_cb;
    void                      *send_ctx;
    raft_apply_cb              apply_cb;
    void                      *apply_ctx;
    raft_event_cb              event_cb;
    void                      *event_ctx;
    raft_snapshot_installed_cb snapshot_cb;
    void                      *snapshot_ctx;

    /* --- Stats (for the dashboard) --- */
    uint64_t elections_started;
    uint64_t entries_committed;
} raft_t;

/* ---- Lifecycle ---- */

/* Initialize. Loads persistent term/voted_for from data_dir/raft_meta.
 * `log` must already be restored (snapshot base + WAL replay). */
int raft_init(raft_t *r, cluster_config_t *config, raft_log_t *log);

void raft_set_send_cb(raft_t *r, raft_send_cb cb, void *ctx);
void raft_set_apply_cb(raft_t *r, raft_apply_cb cb, void *ctx);
void raft_set_event_cb(raft_t *r, raft_event_cb cb, void *ctx);
void raft_set_snapshot_cb(raft_t *r, raft_snapshot_installed_cb cb, void *ctx);

/* Set commit/applied watermarks restored from a snapshot. */
void raft_restore_applied(raft_t *r, uint64_t last_applied);

/* ---- Driving ---- */

/* Call every ~10ms from the event loop. Handles election timeouts,
 * heartbeats, replication, and applying committed entries. */
void raft_tick(raft_t *r);

/* Leader: append a client command. Returns the assigned log index
 * (it is committed later, asynchronously), or 0 if not leader/error. */
uint64_t raft_submit(raft_t *r, uint8_t cmd_type,
                     const char *key, uint32_t key_len,
                     const char *value, uint32_t value_len);

bool raft_is_leader(const raft_t *r);

/* ---- Incoming message dispatch ----
 * `payload` read cursor must be positioned at the RPC payload. */
void raft_handle_message(raft_t *r, uint8_t msg_type, buffer_t *payload);

/* ---- Compaction support ---- */

/* Called by the server after it has written a snapshot of the KV state
 * at `index`. Compacts the in-memory log and the WAL. */
int raft_compact_log(raft_t *r, uint64_t index);

#endif /* KVSTORE_RAFT_H */
