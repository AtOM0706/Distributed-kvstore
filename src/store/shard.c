#include "shard.h"
#include "common/hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING_HASH_SEED 0x5bd1e995u

void hash_ring_init(hash_ring_t *ring)
{
    ring->size = 0;
}

static int vnode_cmp(const void *a, const void *b)
{
    const vnode_t *va = a, *vb = b;
    if (va->hash < vb->hash)
        return -1;
    if (va->hash > vb->hash)
        return 1;
    /* Tie-break by node id so the ring is deterministic on every node */
    return va->node_id - vb->node_id;
}

int hash_ring_add_node(hash_ring_t *ring, int node_id)
{
    if (ring->size + VNODES_PER_NODE > MAX_VNODES)
        return -1;
    for (int i = 0; i < ring->size; i++)
        if (ring->ring[i].node_id == node_id)
            return -1; /* Already present */

    for (int v = 0; v < VNODES_PER_NODE; v++) {
        char label[64];
        int len = snprintf(label, sizeof(label), "node-%d-vnode-%d",
                           node_id, v);
        ring->ring[ring->size].hash =
            murmurhash3_32(label, (size_t)len, RING_HASH_SEED);
        ring->ring[ring->size].node_id = node_id;
        ring->size++;
    }
    qsort(ring->ring, (size_t)ring->size, sizeof(vnode_t), vnode_cmp);
    return 0;
}

void hash_ring_remove_node(hash_ring_t *ring, int node_id)
{
    int w = 0;
    for (int i = 0; i < ring->size; i++) {
        if (ring->ring[i].node_id != node_id)
            ring->ring[w++] = ring->ring[i];
    }
    ring->size = w;
}

int hash_ring_lookup(const hash_ring_t *ring, const char *key,
                     uint32_t key_len)
{
    if (ring->size == 0)
        return -1;

    uint32_t h = murmurhash3_32(key, key_len, RING_HASH_SEED);

    /* First vnode with hash >= h (wrap to 0 if none) */
    int lo = 0, hi = ring->size;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (ring->ring[mid].hash < h)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == ring->size)
        lo = 0; /* Wrap around */
    return ring->ring[lo].node_id;
}

double hash_ring_ownership(const hash_ring_t *ring, int node_id)
{
    if (ring->size == 0)
        return 0.0;

    uint64_t owned = 0;
    for (int i = 0; i < ring->size; i++) {
        /* Segment owned by vnode i: (prev_hash, hash_i] */
        uint32_t prev = ring->ring[(i + ring->size - 1) % ring->size].hash;
        uint32_t cur = ring->ring[i].hash;
        uint64_t span = (i == 0)
                            ? (uint64_t)cur + ((uint64_t)UINT32_MAX + 1 - prev)
                            : (uint64_t)(cur - prev);
        if (ring->ring[i].node_id == node_id)
            owned += span;
    }
    return (double)owned / ((double)UINT32_MAX + 1);
}
