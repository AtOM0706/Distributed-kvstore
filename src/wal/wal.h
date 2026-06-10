#ifndef KVSTORE_WAL_H
#define KVSTORE_WAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * Write-Ahead Log — every Raft log entry is persisted here before being
 * acknowledged. On restart, the WAL is replayed to rebuild in-memory state.
 *
 * On-disk entry format (little-endian):
 *   ┌─────────┬──────┬─────────┬─────────┬──────────┬──────────┬─────────┐
 *   │ magic   │ len  │ term    │ index   │ cmd_type │ payload  │ crc32   │
 *   │ 4 bytes │ 4B   │ 8 bytes │ 8 bytes │ 1 byte   │ variable │ 4 bytes │
 *   └─────────┴──────┴─────────┴─────────┴──────────┴──────────┴─────────┘
 *
 *   - magic:  0x57414C45 ("WALE") marks the start of each entry
 *   - len:    payload length in bytes
 *   - crc32:  checksum over term..payload — detects torn writes/corruption
 *
 * Replay stops (and the file is truncated) at the first corrupt entry,
 * which handles crashes that happen mid-append.
 * ----------------------------------------------------------------------- */

#define WAL_MAGIC        0x57414C45u
#define WAL_HEADER_SIZE  25  /* magic(4) + len(4) + term(8) + index(8) + cmd(1) */
#define WAL_MAX_PAYLOAD  (2 * 1024 * 1024) /* Sanity cap: 2 MB */

/* Command types stored in WAL entries (shared with Raft) */
enum {
    WAL_CMD_SET  = 1,
    WAL_CMD_DEL  = 2,
    WAL_CMD_NOOP = 3,
};

typedef struct {
    int      fd;            /* Open file descriptor (O_APPEND semantics) */
    char     path[512];
    uint64_t last_index;    /* Index of the most recently appended entry */
    uint64_t entry_count;   /* Number of valid entries in the file */
    uint64_t file_size;     /* Current file size in bytes */
    bool     dirty;         /* Appends pending fsync (group commit) */

    /* Index → file offset map, so truncate_after() is O(1) per entry.
     * offsets[i] is the file offset of the i-th entry in the file. */
    struct {
        uint64_t index;     /* Raft index of this entry */
        uint64_t offset;    /* Byte offset in the file */
    } *offsets;
    size_t offsets_count;
    size_t offsets_cap;
} wal_t;

/* Callback invoked for each entry during replay. Return 0 to continue,
 * nonzero to abort replay. */
typedef int (*wal_replay_cb)(uint64_t term, uint64_t index, uint8_t cmd_type,
                             const uint8_t *payload, uint32_t payload_len,
                             void *ctx);

/* Open (or create) the WAL file at `path`. Scans existing entries to
 * rebuild the offset map and detect/trim trailing corruption.
 * Returns 0 on success, -1 on error. */
int wal_open(wal_t *wal, const char *path);

/* Close the WAL and free resources. */
void wal_close(wal_t *wal);

/* Append one entry (buffered — NOT yet durable). Call wal_sync() to make
 * all pending appends durable in one fsync ("group commit").
 * Returns 0 on success. */
int wal_append(wal_t *wal, uint64_t term, uint64_t index, uint8_t cmd_type,
               const void *payload, uint32_t payload_len);

/* fsync() all appends since the last sync. One fsync covers any number of
 * appended entries — this is what makes batched writes fast while still
 * durable before acknowledgement. Returns 0 on success. */
int wal_sync(wal_t *wal);

/* Replay all valid entries in order, invoking `cb` for each.
 * Returns the number of entries replayed, or -1 on error. */
int64_t wal_replay(wal_t *wal, wal_replay_cb cb, void *ctx);

/* Remove all entries with index > `index` (Raft conflict repair).
 * Returns 0 on success. */
int wal_truncate_after(wal_t *wal, uint64_t index);

/* Remove all entries with index <= `index` (log compaction after a
 * snapshot). Implemented as rewrite-to-temp + rename for atomicity.
 * Returns 0 on success. */
int wal_truncate_before(wal_t *wal, uint64_t index);

#endif /* KVSTORE_WAL_H */
