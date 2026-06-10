#ifndef KVSTORE_RAFT_RPC_H
#define KVSTORE_RAFT_RPC_H

#include <stdint.h>
#include <stdbool.h>

#include "common/buffer.h"
#include "raft_log.h"

/* -----------------------------------------------------------------------
 * Raft RPC messages + binary serialization.
 *
 * The transport (net/protocol.c) frames each message as:
 *   [msg_type:1][msg_len:4][payload:N]
 * This module serializes/deserializes the payloads.
 * ----------------------------------------------------------------------- */

/* Message type constants (shared with the client protocol) */
enum {
    MSG_REQUEST_VOTE            = 0x01,
    MSG_REQUEST_VOTE_RESP       = 0x02,
    MSG_APPEND_ENTRIES          = 0x03,
    MSG_APPEND_ENTRIES_RESP     = 0x04,
    MSG_INSTALL_SNAPSHOT        = 0x05,
    MSG_INSTALL_SNAPSHOT_RESP   = 0x06,
    MSG_CLIENT_REQUEST          = 0x10,
    MSG_CLIENT_RESPONSE         = 0x11,
};

#define RAFT_MAX_ENTRIES_PER_AE 64 /* Batch cap per AppendEntries */

typedef struct {
    uint64_t term;
    int32_t  candidate_id;
    uint64_t last_log_index;
    uint64_t last_log_term;
} request_vote_t;

typedef struct {
    uint64_t term;
    bool     vote_granted;
    int32_t  voter_id;
} request_vote_resp_t;

typedef struct {
    uint64_t      term;
    int32_t       leader_id;
    uint64_t      prev_log_index;
    uint64_t      prev_log_term;
    uint64_t      leader_commit;
    uint32_t      num_entries;
    raft_entry_t *entries;       /* Heap array; free with ae_free_entries */
} append_entries_t;

typedef struct {
    uint64_t term;
    bool     success;
    uint64_t match_index;  /* On success: last index follower now has */
    int32_t  follower_id;
} append_entries_resp_t;

typedef struct {
    uint64_t term;
    int32_t  leader_id;
    uint64_t last_included_index;
    uint64_t last_included_term;
    uint64_t offset;       /* Byte offset of this chunk in the snapshot */
    bool     done;         /* True if this is the final chunk */
    uint32_t data_len;
    uint8_t *data;         /* Heap; free after use */
} install_snapshot_t;

typedef struct {
    uint64_t term;
    int32_t  follower_id;
    uint64_t bytes_stored; /* Offset follower expects next (flow control) */
} install_snapshot_resp_t;

/* ---- Serialization (appends payload to buf) ---- */
void rpc_encode_request_vote(buffer_t *buf, const request_vote_t *m);
void rpc_encode_request_vote_resp(buffer_t *buf, const request_vote_resp_t *m);
void rpc_encode_append_entries(buffer_t *buf, const append_entries_t *m);
void rpc_encode_append_entries_resp(buffer_t *buf,
                                    const append_entries_resp_t *m);
void rpc_encode_install_snapshot(buffer_t *buf, const install_snapshot_t *m);
void rpc_encode_install_snapshot_resp(buffer_t *buf,
                                      const install_snapshot_resp_t *m);

/* ---- Deserialization (reads from buf's read cursor) ----
 * Return 0 on success, -1 on malformed input. */
int rpc_decode_request_vote(buffer_t *buf, request_vote_t *m);
int rpc_decode_request_vote_resp(buffer_t *buf, request_vote_resp_t *m);
int rpc_decode_append_entries(buffer_t *buf, append_entries_t *m);
int rpc_decode_append_entries_resp(buffer_t *buf, append_entries_resp_t *m);
int rpc_decode_install_snapshot(buffer_t *buf, install_snapshot_t *m);
int rpc_decode_install_snapshot_resp(buffer_t *buf,
                                     install_snapshot_resp_t *m);

/* Free entries array allocated by rpc_decode_append_entries */
void ae_free_entries(append_entries_t *m);

#endif /* KVSTORE_RAFT_RPC_H */
