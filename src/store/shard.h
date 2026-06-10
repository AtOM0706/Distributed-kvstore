#ifndef KVSTORE_SHARD_H
#define KVSTORE_SHARD_H

#include <stdint.h>
#include <stddef.h>

#include "common/config.h"

/* -----------------------------------------------------------------------
 * Consistent hashing ring with virtual nodes.
 *
 * Each physical node is mapped to VNODES_PER_NODE points on a 32-bit hash
 * ring. A key is owned by the first virtual node clockwise from the key's
 * hash. Virtual nodes smooth the distribution so each physical node owns
 * roughly 1/N of the keyspace.
 *
 * Note: in this 3-node cluster all nodes are one Raft group and store all
 * data, so the ring determines key *ownership* for routing/visualization;
 * it becomes load-bearing when scaling to multiple Raft groups.
 * ----------------------------------------------------------------------- */

#define VNODES_PER_NODE 150
#define MAX_VNODES      (MAX_NODES * VNODES_PER_NODE)

typedef struct {
    uint32_t hash;
    int      node_id;
} vnode_t;

typedef struct {
    vnode_t ring[MAX_VNODES];
    int     size;
} hash_ring_t;

void hash_ring_init(hash_ring_t *ring);

/* Add a physical node (inserts VNODES_PER_NODE virtual nodes).
 * Returns 0 on success, -1 if full or already present. */
int hash_ring_add_node(hash_ring_t *ring, int node_id);

/* Remove a physical node's virtual nodes. */
void hash_ring_remove_node(hash_ring_t *ring, int node_id);

/* Which node owns this key? Binary search, O(log n). Returns -1 if the
 * ring is empty. */
int hash_ring_lookup(const hash_ring_t *ring, const char *key,
                     uint32_t key_len);

/* Fraction of the keyspace owned by `node_id` (0.0 - 1.0), for stats. */
double hash_ring_ownership(const hash_ring_t *ring, int node_id);

#endif /* KVSTORE_SHARD_H */
