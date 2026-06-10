#include "raft_log.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Payload encoding: key_len(4 LE) key value_len(4 LE) value
 * ----------------------------------------------------------------------- */

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t raft_entry_encode(const char *key, uint32_t key_len,
                           const char *value, uint32_t value_len,
                           uint8_t **out)
{
    uint32_t total = 4 + key_len + 4 + value_len;
    uint8_t *buf = malloc(total);
    if (!buf) {
        *out = NULL;
        return 0;
    }
    put_u32(buf, key_len);
    memcpy(buf + 4, key, key_len);
    put_u32(buf + 4 + key_len, value_len);
    if (value_len > 0)
        memcpy(buf + 8 + key_len, value, value_len);
    *out = buf;
    return total;
}

int raft_entry_decode(const uint8_t *payload, uint32_t payload_len,
                      char **key, uint32_t *key_len,
                      char **value, uint32_t *value_len)
{
    *key = *value = NULL;
    *key_len = *value_len = 0;

    if (payload_len < 8)
        return -1;
    uint32_t kl = get_u32(payload);
    if (4 + kl + 4 > payload_len)
        return -1;
    uint32_t vl = get_u32(payload + 4 + kl);
    if (8 + kl + vl != payload_len)
        return -1;

    char *k = malloc(kl + 1);
    char *v = malloc(vl + 1);
    if (!k || !v) {
        free(k);
        free(v);
        return -1;
    }
    memcpy(k, payload + 4, kl);
    k[kl] = '\0';
    memcpy(v, payload + 8 + kl, vl);
    v[vl] = '\0';

    *key = k;
    *key_len = kl;
    *value = v;
    *value_len = vl;
    return 0;
}

/* -----------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */

void raft_log_init(raft_log_t *log, wal_t *wal)
{
    memset(log, 0, sizeof(*log));
    log->wal = wal;
}

static void entry_free(raft_entry_t *e)
{
    free(e->key);
    free(e->value);
    e->key = e->value = NULL;
}

void raft_log_free(raft_log_t *log)
{
    for (size_t i = 0; i < log->count; i++)
        entry_free(&log->entries[i]);
    free(log->entries);
    log->entries = NULL;
    log->count = log->capacity = 0;
}

static int log_reserve(raft_log_t *log, size_t extra)
{
    if (log->count + extra <= log->capacity)
        return 0;
    size_t new_cap = log->capacity ? log->capacity * 2 : 1024;
    while (new_cap < log->count + extra)
        new_cap *= 2;
    void *p = realloc(log->entries, new_cap * sizeof(raft_entry_t));
    if (!p)
        return -1;
    log->entries = p;
    log->capacity = new_cap;
    return 0;
}

/* -----------------------------------------------------------------------
 * Restore from WAL
 * ----------------------------------------------------------------------- */

static int restore_cb(uint64_t term, uint64_t index, uint8_t cmd_type,
                      const uint8_t *payload, uint32_t payload_len, void *ctx)
{
    raft_log_t *log = ctx;

    /* Skip entries already covered by the snapshot */
    if (index <= log->base_index)
        return 0;

    if (log_reserve(log, 1) < 0)
        return -1;

    raft_entry_t *e = &log->entries[log->count];
    memset(e, 0, sizeof(*e));
    e->term = term;
    e->index = index;
    e->cmd_type = cmd_type;

    if (cmd_type != RAFT_CMD_NOOP &&
        raft_entry_decode(payload, payload_len, &e->key, &e->key_len,
                          &e->value, &e->value_len) < 0) {
        LOG_ERROR("raft_log", "Bad payload at index %llu",
                  (unsigned long long)index);
        return -1;
    }
    log->count++;
    return 0;
}

int64_t raft_log_restore(raft_log_t *log)
{
    if (!log->wal)
        return 0;
    int64_t n = wal_replay(log->wal, restore_cb, log);
    if (n >= 0)
        LOG_INFO("raft_log", "Restored %zu entries (base=%llu, last=%llu)",
                 log->count, (unsigned long long)log->base_index,
                 (unsigned long long)raft_log_last_index(log));
    return n;
}

/* -----------------------------------------------------------------------
 * Queries
 * ----------------------------------------------------------------------- */

uint64_t raft_log_last_index(const raft_log_t *log)
{
    return log->count > 0 ? log->entries[log->count - 1].index
                          : log->base_index;
}

uint64_t raft_log_last_term(const raft_log_t *log)
{
    return log->count > 0 ? log->entries[log->count - 1].term
                          : log->base_term;
}

uint64_t raft_log_term_at(const raft_log_t *log, uint64_t index)
{
    if (index == 0)
        return 0;
    if (index == log->base_index)
        return log->base_term;
    if (index < log->base_index || index > raft_log_last_index(log))
        return 0;
    return log->entries[index - log->base_index - 1].term;
}

const raft_entry_t *raft_log_entry_at(const raft_log_t *log, uint64_t index)
{
    if (index <= log->base_index || index > raft_log_last_index(log))
        return NULL;
    return &log->entries[index - log->base_index - 1];
}

/* -----------------------------------------------------------------------
 * Mutations
 * ----------------------------------------------------------------------- */

static int wal_persist_entry(raft_log_t *log, const raft_entry_t *e)
{
    if (!log->wal)
        return 0;

    if (e->cmd_type == RAFT_CMD_NOOP)
        return wal_append(log->wal, e->term, e->index, e->cmd_type, NULL, 0);

    uint8_t *payload;
    uint32_t len = raft_entry_encode(e->key, e->key_len, e->value,
                                     e->value_len, &payload);
    if (!payload)
        return -1;
    int rc = wal_append(log->wal, e->term, e->index, e->cmd_type,
                        payload, len);
    free(payload);
    return rc;
}

uint64_t raft_log_append(raft_log_t *log, uint64_t term, uint8_t cmd_type,
                         const char *key, uint32_t key_len,
                         const char *value, uint32_t value_len)
{
    if (log_reserve(log, 1) < 0)
        return 0;

    raft_entry_t *e = &log->entries[log->count];
    memset(e, 0, sizeof(*e));
    e->term = term;
    e->index = raft_log_last_index(log) + 1;
    e->cmd_type = cmd_type;

    if (cmd_type != RAFT_CMD_NOOP) {
        e->key = malloc(key_len + 1);
        e->value = malloc(value_len + 1);
        if (!e->key || !e->value) {
            entry_free(e);
            return 0;
        }
        memcpy(e->key, key, key_len);
        e->key[key_len] = '\0';
        e->key_len = key_len;
        if (value_len > 0)
            memcpy(e->value, value, value_len);
        e->value[value_len] = '\0';
        e->value_len = value_len;
    }

    if (wal_persist_entry(log, e) < 0) {
        entry_free(e);
        return 0;
    }
    log->count++;
    return e->index;
}

int raft_log_append_existing(raft_log_t *log, const raft_entry_t *entry)
{
    uint64_t expect = raft_log_last_index(log) + 1;
    if (entry->index != expect) {
        LOG_ERROR("raft_log", "Append gap: got %llu, expected %llu",
                  (unsigned long long)entry->index,
                  (unsigned long long)expect);
        return -1;
    }

    if (log_reserve(log, 1) < 0)
        return -1;

    raft_entry_t *e = &log->entries[log->count];
    memset(e, 0, sizeof(*e));
    e->term = entry->term;
    e->index = entry->index;
    e->cmd_type = entry->cmd_type;

    if (entry->cmd_type != RAFT_CMD_NOOP) {
        e->key = malloc(entry->key_len + 1);
        e->value = malloc(entry->value_len + 1);
        if (!e->key || !e->value) {
            entry_free(e);
            return -1;
        }
        memcpy(e->key, entry->key, entry->key_len);
        e->key[entry->key_len] = '\0';
        e->key_len = entry->key_len;
        if (entry->value_len > 0)
            memcpy(e->value, entry->value, entry->value_len);
        e->value[entry->value_len] = '\0';
        e->value_len = entry->value_len;
    }

    if (wal_persist_entry(log, e) < 0) {
        entry_free(e);
        return -1;
    }
    log->count++;
    return 0;
}

int raft_log_truncate_after(raft_log_t *log, uint64_t index)
{
    if (index >= raft_log_last_index(log))
        return 0;
    if (index < log->base_index)
        return -1; /* Cannot truncate into the snapshot */

    size_t keep = (size_t)(index - log->base_index);
    for (size_t i = keep; i < log->count; i++)
        entry_free(&log->entries[i]);
    log->count = keep;

    if (log->wal && wal_truncate_after(log->wal, index) < 0)
        return -1;
    return 0;
}

int raft_log_compact(raft_log_t *log, uint64_t index, uint64_t term)
{
    if (index <= log->base_index)
        return 0;
    uint64_t last = raft_log_last_index(log);
    if (index > last)
        index = last;

    size_t drop = (size_t)(index - log->base_index);
    for (size_t i = 0; i < drop && i < log->count; i++)
        entry_free(&log->entries[i]);
    if (drop < log->count)
        memmove(log->entries, log->entries + drop,
                (log->count - drop) * sizeof(raft_entry_t));
    log->count -= drop;
    log->base_index = index;
    log->base_term = term;

    if (log->wal && wal_truncate_before(log->wal, index) < 0)
        return -1;

    LOG_INFO("raft_log", "Compacted to base_index=%llu (%zu entries remain)",
             (unsigned long long)index, log->count);
    return 0;
}
