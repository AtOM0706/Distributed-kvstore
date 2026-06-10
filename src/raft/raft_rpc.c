#include "raft_rpc.h"

#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * RequestVote
 * ----------------------------------------------------------------------- */

void rpc_encode_request_vote(buffer_t *buf, const request_vote_t *m)
{
    buf_write_u64(buf, m->term);
    buf_write_i32(buf, m->candidate_id);
    buf_write_u64(buf, m->last_log_index);
    buf_write_u64(buf, m->last_log_term);
}

int rpc_decode_request_vote(buffer_t *buf, request_vote_t *m)
{
    if (buf_readable(buf) < 28)
        return -1;
    m->term = buf_read_u64(buf);
    m->candidate_id = buf_read_i32(buf);
    m->last_log_index = buf_read_u64(buf);
    m->last_log_term = buf_read_u64(buf);
    return 0;
}

void rpc_encode_request_vote_resp(buffer_t *buf, const request_vote_resp_t *m)
{
    buf_write_u64(buf, m->term);
    buf_write_u8(buf, m->vote_granted ? 1 : 0);
    buf_write_i32(buf, m->voter_id);
}

int rpc_decode_request_vote_resp(buffer_t *buf, request_vote_resp_t *m)
{
    if (buf_readable(buf) < 13)
        return -1;
    m->term = buf_read_u64(buf);
    m->vote_granted = buf_read_u8(buf) != 0;
    m->voter_id = buf_read_i32(buf);
    return 0;
}

/* -----------------------------------------------------------------------
 * AppendEntries
 * ----------------------------------------------------------------------- */

void rpc_encode_append_entries(buffer_t *buf, const append_entries_t *m)
{
    buf_write_u64(buf, m->term);
    buf_write_i32(buf, m->leader_id);
    buf_write_u64(buf, m->prev_log_index);
    buf_write_u64(buf, m->prev_log_term);
    buf_write_u64(buf, m->leader_commit);
    buf_write_u32(buf, m->num_entries);

    for (uint32_t i = 0; i < m->num_entries; i++) {
        const raft_entry_t *e = &m->entries[i];
        buf_write_u64(buf, e->term);
        buf_write_u64(buf, e->index);
        buf_write_u8(buf, e->cmd_type);
        buf_write_u32(buf, e->key_len);
        if (e->key_len > 0)
            buf_write_bytes(buf, e->key, e->key_len);
        buf_write_u32(buf, e->value_len);
        if (e->value_len > 0)
            buf_write_bytes(buf, e->value, e->value_len);
    }
}

int rpc_decode_append_entries(buffer_t *buf, append_entries_t *m)
{
    memset(m, 0, sizeof(*m));
    if (buf_readable(buf) < 40)
        return -1;

    m->term = buf_read_u64(buf);
    m->leader_id = buf_read_i32(buf);
    m->prev_log_index = buf_read_u64(buf);
    m->prev_log_term = buf_read_u64(buf);
    m->leader_commit = buf_read_u64(buf);
    m->num_entries = buf_read_u32(buf);

    if (m->num_entries > RAFT_MAX_ENTRIES_PER_AE)
        return -1;
    if (m->num_entries == 0)
        return 0;

    m->entries = calloc(m->num_entries, sizeof(raft_entry_t));
    if (!m->entries)
        return -1;

    for (uint32_t i = 0; i < m->num_entries; i++) {
        raft_entry_t *e = &m->entries[i];
        if (buf_readable(buf) < 21)
            goto fail;
        e->term = buf_read_u64(buf);
        e->index = buf_read_u64(buf);
        e->cmd_type = buf_read_u8(buf);

        e->key_len = buf_read_u32(buf);
        if (buf_readable(buf) < e->key_len)
            goto fail;
        e->key = malloc(e->key_len + 1);
        if (!e->key)
            goto fail;
        buf_read_bytes(buf, e->key, e->key_len);
        e->key[e->key_len] = '\0';

        if (buf_readable(buf) < 4)
            goto fail;
        e->value_len = buf_read_u32(buf);
        if (buf_readable(buf) < e->value_len)
            goto fail;
        e->value = malloc(e->value_len + 1);
        if (!e->value)
            goto fail;
        buf_read_bytes(buf, e->value, e->value_len);
        e->value[e->value_len] = '\0';
    }
    return 0;

fail:
    ae_free_entries(m);
    return -1;
}

void ae_free_entries(append_entries_t *m)
{
    if (!m->entries)
        return;
    for (uint32_t i = 0; i < m->num_entries; i++) {
        free(m->entries[i].key);
        free(m->entries[i].value);
    }
    free(m->entries);
    m->entries = NULL;
    m->num_entries = 0;
}

void rpc_encode_append_entries_resp(buffer_t *buf,
                                    const append_entries_resp_t *m)
{
    buf_write_u64(buf, m->term);
    buf_write_u8(buf, m->success ? 1 : 0);
    buf_write_u64(buf, m->match_index);
    buf_write_i32(buf, m->follower_id);
}

int rpc_decode_append_entries_resp(buffer_t *buf, append_entries_resp_t *m)
{
    if (buf_readable(buf) < 21)
        return -1;
    m->term = buf_read_u64(buf);
    m->success = buf_read_u8(buf) != 0;
    m->match_index = buf_read_u64(buf);
    m->follower_id = buf_read_i32(buf);
    return 0;
}

/* -----------------------------------------------------------------------
 * InstallSnapshot
 * ----------------------------------------------------------------------- */

void rpc_encode_install_snapshot(buffer_t *buf, const install_snapshot_t *m)
{
    buf_write_u64(buf, m->term);
    buf_write_i32(buf, m->leader_id);
    buf_write_u64(buf, m->last_included_index);
    buf_write_u64(buf, m->last_included_term);
    buf_write_u64(buf, m->offset);
    buf_write_u8(buf, m->done ? 1 : 0);
    buf_write_u32(buf, m->data_len);
    if (m->data_len > 0)
        buf_write_bytes(buf, m->data, m->data_len);
}

int rpc_decode_install_snapshot(buffer_t *buf, install_snapshot_t *m)
{
    memset(m, 0, sizeof(*m));
    if (buf_readable(buf) < 41)
        return -1;
    m->term = buf_read_u64(buf);
    m->leader_id = buf_read_i32(buf);
    m->last_included_index = buf_read_u64(buf);
    m->last_included_term = buf_read_u64(buf);
    m->offset = buf_read_u64(buf);
    m->done = buf_read_u8(buf) != 0;
    m->data_len = buf_read_u32(buf);
    if (buf_readable(buf) < m->data_len)
        return -1;
    if (m->data_len > 0) {
        m->data = malloc(m->data_len);
        if (!m->data)
            return -1;
        buf_read_bytes(buf, m->data, m->data_len);
    }
    return 0;
}

void rpc_encode_install_snapshot_resp(buffer_t *buf,
                                      const install_snapshot_resp_t *m)
{
    buf_write_u64(buf, m->term);
    buf_write_i32(buf, m->follower_id);
    buf_write_u64(buf, m->bytes_stored);
}

int rpc_decode_install_snapshot_resp(buffer_t *buf,
                                     install_snapshot_resp_t *m)
{
    if (buf_readable(buf) < 20)
        return -1;
    m->term = buf_read_u64(buf);
    m->follower_id = buf_read_i32(buf);
    m->bytes_stored = buf_read_u64(buf);
    return 0;
}
