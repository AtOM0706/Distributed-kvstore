#ifndef KVSTORE_SNAPSHOT_H
#define KVSTORE_SNAPSHOT_H

#include <stdint.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
 * Snapshots — serialize the full KV state for log compaction.
 *
 * After a snapshot at index N is written, all WAL entries with
 * index <= N can be discarded (wal_truncate_before).
 *
 * On-disk format (little-endian):
 *   header:  magic(4) version(4) last_included_index(8)
 *            last_included_term(8) num_entries(8)
 *   entries: key_len(4) key bytes  val_len(4) value bytes   (repeated)
 *   footer:  crc32(4) over everything after the magic
 *
 * Written to a temp file then atomically rename()d into place, so a crash
 * mid-snapshot never corrupts the previous snapshot.
 * ----------------------------------------------------------------------- */

#define SNAPSHOT_MAGIC   0x534E4150u /* "SNAP" */
#define SNAPSHOT_VERSION 1

/* Writer handle — stream entries one at a time to keep memory flat. */
typedef struct snapshot_writer snapshot_writer_t;

/* Begin writing a snapshot. `path` is the final destination.
 * Returns NULL on error. */
snapshot_writer_t *snapshot_begin(const char *path,
                                  uint64_t last_included_index,
                                  uint64_t last_included_term);

/* Add one key-value pair. Returns 0 on success. */
int snapshot_add(snapshot_writer_t *w, const char *key, uint32_t key_len,
                 const char *value, uint32_t value_len);

/* Finalize: patch entry count, write CRC, fsync, atomic rename.
 * Frees the writer. Returns 0 on success. */
int snapshot_commit(snapshot_writer_t *w);

/* Abort and clean up the temp file. Frees the writer. */
void snapshot_abort(snapshot_writer_t *w);

/* Callback invoked for each KV pair during load. Return 0 to continue. */
typedef int (*snapshot_load_cb)(const char *key, uint32_t key_len,
                                const char *value, uint32_t value_len,
                                void *ctx);

/* Load a snapshot, invoking `cb` per entry. Outputs the metadata.
 * Returns number of entries loaded, -1 on error/corruption,
 * 0 with *out_index == 0 if the file does not exist. */
int64_t snapshot_load(const char *path, snapshot_load_cb cb, void *ctx,
                      uint64_t *out_last_index, uint64_t *out_last_term);

#endif /* KVSTORE_SNAPSHOT_H */
