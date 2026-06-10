/* Unit tests: MurmurHash3, CRC32, and the consistent hashing ring. */

#include "common/hash.h"
#include "store/shard.h"

#undef NDEBUG /* Tests rely on assert() even in Release builds */
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_murmur(void)
{
    /* Same input + seed → same hash; different seeds → different hash */
    uint32_t a = murmurhash3_32("hello", 5, 0);
    uint32_t b = murmurhash3_32("hello", 5, 0);
    uint32_t c = murmurhash3_32("hello", 5, 1);
    uint32_t d = murmurhash3_32("hellp", 5, 0);
    assert(a == b);
    assert(a != c);
    assert(a != d);
    printf("  ok: murmurhash3 determinism\n");
}

static void test_crc(void)
{
    /* Known vector: CRC32("123456789") = 0xCBF43926 */
    assert(crc32("123456789", 9) == 0xCBF43926u);
    printf("  ok: crc32 known vector\n");
}

static void test_ring_distribution(void)
{
    hash_ring_t ring;
    hash_ring_init(&ring);
    for (int n = 1; n <= 3; n++)
        assert(hash_ring_add_node(&ring, n) == 0);
    assert(ring.size == 3 * VNODES_PER_NODE);

    /* Adding a duplicate must fail */
    assert(hash_ring_add_node(&ring, 2) == -1);

    /* 30k keys should split roughly evenly (25%–42% per node) */
    int counts[4] = {0};
    char key[32];
    for (int i = 0; i < 30000; i++) {
        int kl = snprintf(key, sizeof(key), "k%d", i);
        int owner = hash_ring_lookup(&ring, key, (uint32_t)kl);
        assert(owner >= 1 && owner <= 3);
        counts[owner]++;
    }
    for (int n = 1; n <= 3; n++) {
        double frac = counts[n] / 30000.0;
        assert(frac > 0.25 && frac < 0.42);
    }
    printf("  ok: distribution %d/%d/%d across 3 nodes\n", counts[1],
           counts[2], counts[3]);

    /* Ownership fractions sum to ~1 */
    double total = 0;
    for (int n = 1; n <= 3; n++)
        total += hash_ring_ownership(&ring, n);
    assert(total > 0.999 && total < 1.001);
    printf("  ok: ownership fractions sum to 1\n");
}

static void test_ring_remove(void)
{
    hash_ring_t ring;
    hash_ring_init(&ring);
    for (int n = 1; n <= 3; n++)
        hash_ring_add_node(&ring, n);

    /* Record ownership before removal */
    char key[32];
    int before[1000];
    for (int i = 0; i < 1000; i++) {
        int kl = snprintf(key, sizeof(key), "k%d", i);
        before[i] = hash_ring_lookup(&ring, key, (uint32_t)kl);
    }

    hash_ring_remove_node(&ring, 2);
    assert(ring.size == 2 * VNODES_PER_NODE);

    /* Keys owned by surviving nodes must NOT move (the point of
     * consistent hashing); keys owned by node 2 must move to 1 or 3. */
    int moved = 0;
    for (int i = 0; i < 1000; i++) {
        int kl = snprintf(key, sizeof(key), "k%d", i);
        int now = hash_ring_lookup(&ring, key, (uint32_t)kl);
        assert(now == 1 || now == 3);
        if (before[i] != 2)
            assert(now == before[i]);
        else
            moved++;
    }
    printf("  ok: node removal moved only its own keys (%d/1000)\n", moved);
}

int main(void)
{
    printf("test_hash:\n");
    test_murmur();
    test_crc();
    test_ring_distribution();
    test_ring_remove();
    printf("ALL HASH TESTS PASSED\n");
    return 0;
}
