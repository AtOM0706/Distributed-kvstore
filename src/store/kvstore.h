#ifndef KVSTORE_KVSTORE_H
#define KVSTORE_KVSTORE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>

/* -----------------------------------------------------------------------
 * In-memory hash table with separate chaining.
 *
 * Thread safety: a read-write lock guards all operations, so the
 * dashboard/WebSocket thread can read stats while the event loop writes.
 * (In the single-threaded event loop the lock is uncontended and cheap.)
 * ----------------------------------------------------------------------- */

#define KV_INITIAL_CAPACITY 65536
#define KV_MAX_LOAD_FACTOR  0.75

typedef struct kv_entry {
    char            *key;
    uint32_t         key_len;
    char            *value;
    uint32_t         value_len;
    struct kv_entry *next;     /* Collision chain */
} kv_entry_t;

typedef struct {
    kv_entry_t      **buckets;
    size_t            capacity;   /* Always a power of two */
    size_t            count;
    pthread_rwlock_t  lock;
    /* Stats */
    uint64_t          total_sets;
    uint64_t          total_gets;
    uint64_t          total_dels;
} kvstore_t;

int  kvstore_init(kvstore_t *kv);
void kvstore_free(kvstore_t *kv);

/* Insert or update. Copies key and value. Returns 0 on success. */
int kvstore_set(kvstore_t *kv, const char *key, uint32_t key_len,
                const char *value, uint32_t value_len);

/* Look up a key. On hit, *value and *value_len point to internal storage
 * (valid until the next mutation — copy if you need to keep it).
 * Returns true on hit. */
bool kvstore_get(kvstore_t *kv, const char *key, uint32_t key_len,
                 const char **value, uint32_t *value_len);

/* Delete a key. Returns true if it existed. */
bool kvstore_del(kvstore_t *kv, const char *key, uint32_t key_len);

/* Remove every entry (used when installing a snapshot from the leader). */
void kvstore_clear(kvstore_t *kv);

size_t kvstore_count(kvstore_t *kv);

/* Iterate all entries (e.g. to write a snapshot). Callback returns 0 to
 * continue. Holds the read lock for the duration — do not mutate inside. */
typedef int (*kvstore_iter_cb)(const char *key, uint32_t key_len,
                               const char *value, uint32_t value_len,
                               void *ctx);
void kvstore_foreach(kvstore_t *kv, kvstore_iter_cb cb, void *ctx);

#endif /* KVSTORE_KVSTORE_H */
