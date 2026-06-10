#include "raft.h"
#include "common/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define SNAPSHOT_CHUNK_SIZE (64 * 1024)

const char *raft_role_name(raft_role_t role)
{
    switch (role) {
    case RAFT_FOLLOWER:  return "follower";
    case RAFT_CANDIDATE: return "candidate";
    case RAFT_LEADER:    return "leader";
    }
    return "unknown";
}

static void emit_event(raft_t *r, const char *type, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void emit_event(raft_t *r, const char *type, const char *fmt, ...)
{
    if (!r->event_cb)
        return;
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    r->event_cb(type, msg, r->event_ctx);
}

/* -----------------------------------------------------------------------
 * Persistent meta state: current_term + voted_for.
 * Written atomically (tmp + fsync + rename) on every change, as required
 * by the Raft safety argument.
 * ----------------------------------------------------------------------- */

static int meta_save(raft_t *r)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", r->meta_path);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;

    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%llu %d\n",
                       (unsigned long long)r->current_term, r->voted_for);
    if (write(fd, buf, (size_t)len) != len || fsync(fd) < 0) {
        close(fd);
        unlink(tmp);
        return -1;
    }
    close(fd);

    if (rename(tmp, r->meta_path) < 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static void meta_load(raft_t *r)
{
    FILE *f = fopen(r->meta_path, "r");
    if (!f)
        return;
    unsigned long long term;
    int voted;
    if (fscanf(f, "%llu %d", &term, &voted) == 2) {
        r->current_term = term;
        r->voted_for = voted;
    }
    fclose(f);
}

/* -----------------------------------------------------------------------
 * Role transitions
 * ----------------------------------------------------------------------- */

static void become_follower(raft_t *r, uint64_t term)
{
    bool was_leader = (r->role == RAFT_LEADER);
    if (term > r->current_term) {
        r->current_term = term;
        r->voted_for = -1;
        meta_save(r);
    }
    r->role = RAFT_FOLLOWER;
    r->votes_received = 0;
    timer_reset_election(&r->timer);
    if (was_leader) {
        LOG_INFO("raft", "Stepping down to follower (term %llu)",
                 (unsigned long long)r->current_term);
        emit_event(r, "election", "Node %d stepped down (term %llu)",
                   r->config->node_id, (unsigned long long)r->current_term);
    }
}

static void replicate_to_peer(raft_t *r, int peer_id);

static void become_leader(raft_t *r)
{
    r->role = RAFT_LEADER;
    r->leader_id = r->config->node_id;

    uint64_t last = raft_log_last_index(r->log);
    for (int i = 0; i <= MAX_NODES; i++) {
        r->peers[i].next_index = last + 1;
        r->peers[i].match_index = 0;
        r->peers[i].snapshot_inflight = false;
        r->peers[i].snapshot_offset = 0;
    }

    LOG_INFO("raft", "Node %d elected LEADER for term %llu",
             r->config->node_id, (unsigned long long)r->current_term);
    emit_event(r, "election", "Node %d elected leader (term %llu)",
               r->config->node_id, (unsigned long long)r->current_term);

    /* Commit a no-op to establish leadership over prior-term entries
     * (Raft §8: a leader may only commit entries from its own term by
     * counting replicas — the no-op forces that quickly). */
    raft_log_append(r->log, r->current_term, RAFT_CMD_NOOP, NULL, 0, NULL, 0);

    /* Immediately announce leadership */
    for (int i = 0; i < r->config->num_peers; i++)
        replicate_to_peer(r, r->config->peers[i].node_id);
}

static void start_election(raft_t *r)
{
    r->role = RAFT_CANDIDATE;
    r->current_term++;
    r->voted_for = r->config->node_id;
    meta_save(r);
    r->votes_received = 1; /* Vote for self */
    r->leader_id = -1;
    r->elections_started++;
    timer_reset_election(&r->timer);

    LOG_INFO("raft", "Node %d starting election for term %llu",
             r->config->node_id, (unsigned long long)r->current_term);
    emit_event(r, "election", "Node %d started election (term %llu)",
               r->config->node_id, (unsigned long long)r->current_term);

    request_vote_t req = {
        .term = r->current_term,
        .candidate_id = r->config->node_id,
        .last_log_index = raft_log_last_index(r->log),
        .last_log_term = raft_log_last_term(r->log),
    };

    buffer_t buf;
    buf_init(&buf, 64);
    rpc_encode_request_vote(&buf, &req);
    for (int i = 0; i < r->config->num_peers; i++)
        r->send_cb(r->config->peers[i].node_id, MSG_REQUEST_VOTE, &buf,
                   r->send_ctx);
    buf_free(&buf);
}

/* Majority size for the full cluster (peers + self) */
static int majority(const raft_t *r)
{
    return (r->config->num_peers + 1) / 2 + 1;
}

/* -----------------------------------------------------------------------
 * Replication (leader)
 * ----------------------------------------------------------------------- */

static void send_snapshot_chunk(raft_t *r, int peer_id)
{
    raft_peer_t *p = &r->peers[peer_id];

    int fd = open(r->snapshot_path, O_RDONLY);
    if (fd < 0) {
        LOG_ERROR("raft", "Cannot open snapshot for peer %d: %s", peer_id,
                  strerror(errno));
        p->snapshot_inflight = false;
        return;
    }
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return;
    }
    uint64_t size = (uint64_t)st.st_size;
    uint64_t off = p->snapshot_offset;
    if (off > size)
        off = size;
    uint32_t chunk = (uint32_t)((size - off) < SNAPSHOT_CHUNK_SIZE
                                    ? (size - off) : SNAPSHOT_CHUNK_SIZE);

    install_snapshot_t msg = {
        .term = r->current_term,
        .leader_id = r->config->node_id,
        .last_included_index = r->log->base_index,
        .last_included_term = r->log->base_term,
        .offset = off,
        .done = (off + chunk >= size),
        .data_len = chunk,
        .data = NULL,
    };
    if (chunk > 0) {
        msg.data = malloc(chunk);
        if (!msg.data || pread(fd, msg.data, chunk, (off_t)off) != chunk) {
            free(msg.data);
            close(fd);
            return;
        }
    }
    close(fd);

    buffer_t buf;
    buf_init(&buf, 64 + chunk);
    rpc_encode_install_snapshot(&buf, &msg);
    r->send_cb(peer_id, MSG_INSTALL_SNAPSHOT, &buf, r->send_ctx);
    buf_free(&buf);
    free(msg.data);

    LOG_DEBUG("raft", "Sent snapshot chunk to %d: off=%llu len=%u done=%d",
              peer_id, (unsigned long long)off, chunk, msg.done);
}

static void replicate_to_peer(raft_t *r, int peer_id)
{
    raft_peer_t *p = &r->peers[peer_id];

    if (p->snapshot_inflight) {
        /* Chunks are driven by InstallSnapshotResp; resend current chunk
         * from the heartbeat path only as a retry. */
        send_snapshot_chunk(r, peer_id);
        return;
    }

    /* If the peer needs entries we've compacted away, fall back to
     * snapshot transfer. */
    if (p->next_index <= r->log->base_index) {
        p->snapshot_inflight = true;
        p->snapshot_offset = 0;
        emit_event(r, "replication", "Node %d sending snapshot to node %d",
                   r->config->node_id, peer_id);
        send_snapshot_chunk(r, peer_id);
        return;
    }

    uint64_t prev_index = p->next_index - 1;
    append_entries_t ae = {
        .term = r->current_term,
        .leader_id = r->config->node_id,
        .prev_log_index = prev_index,
        .prev_log_term = raft_log_term_at(r->log, prev_index),
        .leader_commit = r->commit_index,
        .num_entries = 0,
        .entries = NULL,
    };

    /* Batch entries [next_index, last] up to the cap */
    uint64_t last = raft_log_last_index(r->log);
    raft_entry_t batch[RAFT_MAX_ENTRIES_PER_AE];
    for (uint64_t idx = p->next_index;
         idx <= last && ae.num_entries < RAFT_MAX_ENTRIES_PER_AE; idx++) {
        const raft_entry_t *e = raft_log_entry_at(r->log, idx);
        if (!e)
            break;
        batch[ae.num_entries++] = *e; /* Shallow copy — encode only reads */
    }
    ae.entries = batch;

    buffer_t buf;
    buf_init(&buf, 256);
    rpc_encode_append_entries(&buf, &ae);
    r->send_cb(peer_id, MSG_APPEND_ENTRIES, &buf, r->send_ctx);
    buf_free(&buf);
    p->last_ae_sent_ms = time_now_ms();
}

static void leader_broadcast(raft_t *r)
{
    for (int i = 0; i < r->config->num_peers; i++)
        replicate_to_peer(r, r->config->peers[i].node_id);
}

/* Advance commit_index: find the highest N such that a majority of
 * match_index >= N and log[N].term == current_term (Raft §5.4.2). */
static void advance_commit(raft_t *r)
{
    uint64_t last = raft_log_last_index(r->log);
    for (uint64_t n = last; n > r->commit_index; n--) {
        if (raft_log_term_at(r->log, n) != r->current_term)
            break; /* Older terms can't be committed by counting */

        int count = 1; /* Self */
        for (int i = 0; i < r->config->num_peers; i++) {
            int pid = r->config->peers[i].node_id;
            if (r->peers[pid].match_index >= n)
                count++;
        }
        if (count >= majority(r)) {
            r->commit_index = n;
            break;
        }
    }
}

/* Apply newly committed entries to the state machine. */
static void apply_committed(raft_t *r)
{
    while (r->last_applied < r->commit_index) {
        uint64_t next = r->last_applied + 1;
        const raft_entry_t *e = raft_log_entry_at(r->log, next);
        if (!e) {
            /* Entry compacted — state already covered by snapshot. */
            r->last_applied = next;
            continue;
        }
        if (e->cmd_type != RAFT_CMD_NOOP && r->apply_cb)
            r->apply_cb(e, r->apply_ctx);
        r->last_applied = next;
        r->entries_committed++;
    }
}

/* -----------------------------------------------------------------------
 * RPC handlers
 * ----------------------------------------------------------------------- */

static void handle_request_vote(raft_t *r, buffer_t *payload)
{
    request_vote_t req;
    if (rpc_decode_request_vote(payload, &req) < 0)
        return;

    if (req.term > r->current_term)
        become_follower(r, req.term);

    bool grant = false;
    if (req.term == r->current_term &&
        (r->voted_for == -1 || r->voted_for == req.candidate_id)) {
        /* Election restriction (§5.4.1): candidate's log must be at least
         * as up-to-date as ours. */
        uint64_t my_last_term = raft_log_last_term(r->log);
        uint64_t my_last_index = raft_log_last_index(r->log);
        bool up_to_date =
            (req.last_log_term > my_last_term) ||
            (req.last_log_term == my_last_term &&
             req.last_log_index >= my_last_index);
        if (up_to_date) {
            grant = true;
            r->voted_for = req.candidate_id;
            meta_save(r);
            timer_reset_election(&r->timer);
        }
    }

    LOG_DEBUG("raft", "Vote request from %d (term %llu): %s",
              req.candidate_id, (unsigned long long)req.term,
              grant ? "GRANTED" : "denied");
    if (grant)
        emit_event(r, "election", "Node %d voted for node %d (term %llu)",
                   r->config->node_id, req.candidate_id,
                   (unsigned long long)req.term);

    request_vote_resp_t resp = {
        .term = r->current_term,
        .vote_granted = grant,
        .voter_id = r->config->node_id,
    };
    buffer_t buf;
    buf_init(&buf, 16);
    rpc_encode_request_vote_resp(&buf, &resp);
    r->send_cb(req.candidate_id, MSG_REQUEST_VOTE_RESP, &buf, r->send_ctx);
    buf_free(&buf);
}

static void handle_request_vote_resp(raft_t *r, buffer_t *payload)
{
    request_vote_resp_t resp;
    if (rpc_decode_request_vote_resp(payload, &resp) < 0)
        return;

    if (resp.term > r->current_term) {
        become_follower(r, resp.term);
        return;
    }
    if (r->role != RAFT_CANDIDATE || resp.term != r->current_term)
        return;

    if (resp.vote_granted) {
        r->votes_received++;
        LOG_DEBUG("raft", "Vote from %d (%d/%d)", resp.voter_id,
                  r->votes_received, majority(r));
        if (r->votes_received >= majority(r))
            become_leader(r);
    }
}

static void handle_append_entries(raft_t *r, buffer_t *payload)
{
    append_entries_t ae;
    if (rpc_decode_append_entries(payload, &ae) < 0)
        return;

    append_entries_resp_t resp = {
        .term = r->current_term,
        .success = false,
        .match_index = raft_log_last_index(r->log),
        .follower_id = r->config->node_id,
    };

    if (ae.term < r->current_term)
        goto reply; /* Stale leader */

    /* Valid leader for this term */
    if (ae.term > r->current_term || r->role != RAFT_FOLLOWER)
        become_follower(r, ae.term);
    r->leader_id = ae.leader_id;
    timer_reset_election(&r->timer);
    resp.term = r->current_term;

    /* Log consistency check */
    if (ae.prev_log_index > 0) {
        uint64_t local_term = raft_log_term_at(r->log, ae.prev_log_index);
        if (ae.prev_log_index > raft_log_last_index(r->log) ||
            (local_term != 0 && local_term != ae.prev_log_term) ||
            (local_term == 0 && ae.prev_log_index > r->log->base_index)) {
            LOG_DEBUG("raft", "AE consistency fail: prev=%llu/%llu local_term=%llu",
                      (unsigned long long)ae.prev_log_index,
                      (unsigned long long)ae.prev_log_term,
                      (unsigned long long)local_term);
            resp.match_index = raft_log_last_index(r->log);
            goto reply;
        }
    }

    /* Append entries, resolving conflicts */
    for (uint32_t i = 0; i < ae.num_entries; i++) {
        raft_entry_t *e = &ae.entries[i];
        uint64_t local_term = raft_log_term_at(r->log, e->index);
        if (local_term == e->term)
            continue; /* Already have it */
        if (local_term != 0) {
            /* Conflict: delete this entry and everything after (§5.3) */
            raft_log_truncate_after(r->log, e->index - 1);
        }
        if (raft_log_append_existing(r->log, e) < 0)
            goto reply;
    }

    /* Group commit: one fsync covers every entry in this AE batch.
     * Entries must be durable BEFORE we acknowledge them (§5.3). */
    if (ae.num_entries > 0 && r->log->wal && wal_sync(r->log->wal) < 0)
        goto reply;

    resp.success = true;
    resp.match_index = ae.num_entries > 0
                           ? ae.entries[ae.num_entries - 1].index
                           : ae.prev_log_index;

    /* Advance commit index */
    if (ae.leader_commit > r->commit_index) {
        uint64_t last = raft_log_last_index(r->log);
        r->commit_index = ae.leader_commit < last ? ae.leader_commit : last;
        apply_committed(r);
    }

reply:
    ae_free_entries(&ae);
    {
        buffer_t buf;
        buf_init(&buf, 32);
        rpc_encode_append_entries_resp(&buf, &resp);
        r->send_cb(ae.leader_id, MSG_APPEND_ENTRIES_RESP, &buf, r->send_ctx);
        buf_free(&buf);
    }
}

static void handle_append_entries_resp(raft_t *r, buffer_t *payload)
{
    append_entries_resp_t resp;
    if (rpc_decode_append_entries_resp(payload, &resp) < 0)
        return;

    if (resp.term > r->current_term) {
        become_follower(r, resp.term);
        return;
    }
    if (r->role != RAFT_LEADER || resp.term != r->current_term)
        return;

    raft_peer_t *p = &r->peers[resp.follower_id];

    if (resp.success) {
        if (resp.match_index > p->match_index)
            p->match_index = resp.match_index;
        p->next_index = p->match_index + 1;
        advance_commit(r);
        apply_committed(r);
        /* If the follower is still behind, keep pushing */
        if (p->next_index <= raft_log_last_index(r->log))
            replicate_to_peer(r, resp.follower_id);
    } else {
        /* Back off using the follower's hint (its last log index) */
        uint64_t hint = resp.match_index + 1;
        uint64_t backed = p->next_index > 1 ? p->next_index - 1 : 1;
        p->next_index = hint < backed ? hint : backed;
        if (p->next_index < 1)
            p->next_index = 1;
        replicate_to_peer(r, resp.follower_id);
    }
}

static void handle_install_snapshot(raft_t *r, buffer_t *payload)
{
    install_snapshot_t msg;
    if (rpc_decode_install_snapshot(payload, &msg) < 0)
        return;

    install_snapshot_resp_t resp = {
        .term = r->current_term,
        .follower_id = r->config->node_id,
        .bytes_stored = 0,
    };

    if (msg.term < r->current_term)
        goto reply;

    if (msg.term > r->current_term || r->role != RAFT_FOLLOWER)
        become_follower(r, msg.term);
    r->leader_id = msg.leader_id;
    timer_reset_election(&r->timer);
    resp.term = r->current_term;

    /* Write chunk to the staging file */
    char staging[600];
    snprintf(staging, sizeof(staging), "%s.incoming", r->snapshot_path);

    int flags = O_WRONLY | O_CREAT;
    if (msg.offset == 0)
        flags |= O_TRUNC;
    int fd = open(staging, flags, 0644);
    if (fd < 0)
        goto reply;
    if (msg.data_len > 0 &&
        pwrite(fd, msg.data, msg.data_len, (off_t)msg.offset) !=
            (ssize_t)msg.data_len) {
        close(fd);
        goto reply;
    }
    fsync(fd);
    close(fd);
    resp.bytes_stored = msg.offset + msg.data_len;

    if (msg.done) {
        if (rename(staging, r->snapshot_path) < 0)
            goto reply;

        LOG_INFO("raft", "Snapshot installed: last_index=%llu",
                 (unsigned long long)msg.last_included_index);
        emit_event(r, "replication", "Node %d installed snapshot (index %llu)",
                   r->config->node_id,
                   (unsigned long long)msg.last_included_index);

        /* Reset the log to start after the snapshot */
        raft_log_truncate_after(r->log, r->log->base_index);
        raft_log_free(r->log);
        r->log->base_index = msg.last_included_index;
        r->log->base_term = msg.last_included_term;
        if (r->log->wal)
            wal_truncate_after(r->log->wal, 0); /* Clear the entire WAL */

        r->commit_index = msg.last_included_index;
        r->last_applied = msg.last_included_index;

        /* Tell the server to reload its KV store from the file */
        if (r->snapshot_cb)
            r->snapshot_cb(r->snapshot_path, msg.last_included_index,
                           msg.last_included_term, r->snapshot_ctx);
    }

reply:
    free(msg.data);
    {
        buffer_t buf;
        buf_init(&buf, 32);
        rpc_encode_install_snapshot_resp(&buf, &resp);
        r->send_cb(msg.leader_id, MSG_INSTALL_SNAPSHOT_RESP, &buf,
                   r->send_ctx);
        buf_free(&buf);
    }
}

static void handle_install_snapshot_resp(raft_t *r, buffer_t *payload)
{
    install_snapshot_resp_t resp;
    if (rpc_decode_install_snapshot_resp(payload, &resp) < 0)
        return;

    if (resp.term > r->current_term) {
        become_follower(r, resp.term);
        return;
    }
    if (r->role != RAFT_LEADER)
        return;

    raft_peer_t *p = &r->peers[resp.follower_id];
    if (!p->snapshot_inflight)
        return;

    struct stat st;
    if (stat(r->snapshot_path, &st) < 0) {
        p->snapshot_inflight = false;
        return;
    }

    if (resp.bytes_stored >= (uint64_t)st.st_size) {
        /* Transfer complete */
        p->snapshot_inflight = false;
        p->match_index = r->log->base_index;
        p->next_index = r->log->base_index + 1;
        LOG_INFO("raft", "Snapshot transfer to node %d complete",
                 resp.follower_id);
        replicate_to_peer(r, resp.follower_id);
    } else {
        p->snapshot_offset = resp.bytes_stored;
        send_snapshot_chunk(r, resp.follower_id);
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

int raft_init(raft_t *r, cluster_config_t *config, raft_log_t *log)
{
    memset(r, 0, sizeof(*r));
    r->config = config;
    r->log = log;
    r->voted_for = -1;
    r->leader_id = -1;
    r->role = RAFT_FOLLOWER;

    snprintf(r->meta_path, sizeof(r->meta_path), "%s/raft_meta",
             config->data_dir);
    snprintf(r->snapshot_path, sizeof(r->snapshot_path), "%s/snapshot.db",
             config->data_dir);

    meta_load(r);

    timer_init(&r->timer, config->election_timeout_min_ms,
               config->election_timeout_max_ms,
               config->heartbeat_interval_ms);

    LOG_INFO("raft", "Initialized node %d: term=%llu voted_for=%d "
             "log_last=%llu", config->node_id,
             (unsigned long long)r->current_term, r->voted_for,
             (unsigned long long)raft_log_last_index(log));
    return 0;
}

void raft_set_send_cb(raft_t *r, raft_send_cb cb, void *ctx)
{
    r->send_cb = cb;
    r->send_ctx = ctx;
}

void raft_set_apply_cb(raft_t *r, raft_apply_cb cb, void *ctx)
{
    r->apply_cb = cb;
    r->apply_ctx = ctx;
}

void raft_set_event_cb(raft_t *r, raft_event_cb cb, void *ctx)
{
    r->event_cb = cb;
    r->event_ctx = ctx;
}

void raft_set_snapshot_cb(raft_t *r, raft_snapshot_installed_cb cb, void *ctx)
{
    r->snapshot_cb = cb;
    r->snapshot_ctx = ctx;
}

void raft_restore_applied(raft_t *r, uint64_t last_applied)
{
    r->last_applied = last_applied;
    if (r->commit_index < last_applied)
        r->commit_index = last_applied;
}

bool raft_is_leader(const raft_t *r)
{
    return r->role == RAFT_LEADER;
}

void raft_tick(raft_t *r)
{
    /* Group commit: one fsync for everything appended since last tick */
    if (r->log->wal)
        wal_sync(r->log->wal);

    if (r->role == RAFT_LEADER) {
        if (r->replication_pending || timer_heartbeat_due(&r->timer)) {
            r->replication_pending = false;
            leader_broadcast(r);
        }
    } else {
        if (timer_election_expired(&r->timer))
            start_election(r);
    }
    apply_committed(r);
}

uint64_t raft_submit(raft_t *r, uint8_t cmd_type,
                     const char *key, uint32_t key_len,
                     const char *value, uint32_t value_len)
{
    if (r->role != RAFT_LEADER)
        return 0;

    uint64_t index = raft_log_append(r->log, r->current_term, cmd_type,
                                     key, key_len, value, value_len);
    if (index == 0)
        return 0;

    /* Batched replication: flag it; raft_tick() (which runs right after
     * the current poll iteration) sends ONE AppendEntries covering every
     * entry submitted in this batch — and one fsync covers them all. */
    r->replication_pending = true;

    /* Single-node cluster: commit immediately */
    if (r->config->num_peers == 0) {
        if (r->log->wal)
            wal_sync(r->log->wal);
        advance_commit(r);
        apply_committed(r);
    }
    return index;
}

void raft_handle_message(raft_t *r, uint8_t msg_type, buffer_t *payload)
{
    switch (msg_type) {
    case MSG_REQUEST_VOTE:
        handle_request_vote(r, payload);
        break;
    case MSG_REQUEST_VOTE_RESP:
        handle_request_vote_resp(r, payload);
        break;
    case MSG_APPEND_ENTRIES:
        handle_append_entries(r, payload);
        break;
    case MSG_APPEND_ENTRIES_RESP:
        handle_append_entries_resp(r, payload);
        break;
    case MSG_INSTALL_SNAPSHOT:
        handle_install_snapshot(r, payload);
        break;
    case MSG_INSTALL_SNAPSHOT_RESP:
        handle_install_snapshot_resp(r, payload);
        break;
    default:
        LOG_WARN("raft", "Unknown message type 0x%02x", msg_type);
    }
}

int raft_compact_log(raft_t *r, uint64_t index)
{
    uint64_t term = raft_log_term_at(r->log, index);
    if (term == 0)
        return -1;
    return raft_log_compact(r->log, index, term);
}
