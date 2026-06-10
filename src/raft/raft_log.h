#ifndef KVSTORE_RAFT_LOG_H
#define KVSTORE_RAFT_LOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "wal/wal.h"

/* -----------------------------------------------------------------------
 * Raft log — in-memory array of entries, durably backed by the WAL.
 *
 * Indexing: Raft indices are 1-based. After snapshot compaction,
 * entries[0] corresponds to Raft index base_index + 1.
 * ----------------------------------------------------------------------- */

/* Command types (aliases of the WAL constants) */
#define RAFT_CMD_SET  WAL_CMD_SET
#define RAFT_CMD_DEL  WAL_CMD_DEL
#define RAFT_CMD_NOOP WAL_CMD_NOOP

typedef struct {
    uint64_t term;
    uint64_t index;
    uint8_t  cmd_type;
    char    *key;        /* Heap-allocated, NUL-terminated */
    uint32_t key_len;
    char    *value;      /* Heap-allocated, NUL-terminated (may be NULL) */
    uint32_t value_len;
} raft_entry_t;

typedef struct {
    raft_entry_t *entries;
    size_t        count;
    size_t        capacity;
    uint64_t      base_index;     /* Highest index compacted into snapshot */
    uint64_t      base_term;      /* Term of the entry at base_index */
    wal_t        *wal;            /* NULL in unit tests (no persistence) */
} raft_log_t;

/* ---- Lifecycle ---- */
void raft_log_init(raft_log_t *log, wal_t *wal);
void raft_log_free(raft_log_t *log);

/* Restore in-memory entries by replaying the WAL. Call once at startup,
 * after setting base_index/base_term from the snapshot (if any).
 * Returns number of entries restored, -1 on error. */
int64_t raft_log_restore(raft_log_t *log);

/* ---- Queries ---- */
uint64_t raft_log_last_index(const raft_log_t *log);
uint64_t raft_log_last_term(const raft_log_t *log);

/* Term of the entry at `index`. Returns base_term for base_index,
 * 0 if the index is 0 or out of range. */
uint64_t raft_log_term_at(const raft_log_t *log, uint64_t index);

/* Entry at Raft index `index`, or NULL if compacted/out of range. */
const raft_entry_t *raft_log_entry_at(const raft_log_t *log, uint64_t index);

/* ---- Mutations ---- */

/* Leader: append a brand-new entry with the given term. Assigns the next
 * index, persists to WAL. Returns the assigned index, or 0 on error. */
uint64_t raft_log_append(raft_log_t *log, uint64_t term, uint8_t cmd_type,
                         const char *key, uint32_t key_len,
                         const char *value, uint32_t value_len);

/* Follower: append an entry with explicit term+index (from AppendEntries).
 * Caller must have already resolved conflicts. Returns 0 on success. */
int raft_log_append_existing(raft_log_t *log, const raft_entry_t *entry);

/* Delete all entries with index > `index` (in memory and WAL). */
int raft_log_truncate_after(raft_log_t *log, uint64_t index);

/* Discard all entries with index <= `index` after a snapshot.
 * `term` is the term of the entry at `index`. */
int raft_log_compact(raft_log_t *log, uint64_t index, uint64_t term);

/* ---- Helpers for WAL payload encoding (key/value <-> bytes) ---- */

/* Encode key+value into `out` (malloc'd, caller frees). Returns length. */
uint32_t raft_entry_encode(const char *key, uint32_t key_len,
                           const char *value, uint32_t value_len,
                           uint8_t **out);

/* Decode payload into key/value (malloc'd, NUL-terminated, caller frees).
 * Returns 0 on success. */
int raft_entry_decode(const uint8_t *payload, uint32_t payload_len,
                      char **key, uint32_t *key_len,
                      char **value, uint32_t *value_len);

#endif /* KVSTORE_RAFT_LOG_H */
