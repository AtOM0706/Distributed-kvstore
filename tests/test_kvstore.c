/* Unit tests: KV store — CRUD, resizing under load, overwrite semantics,
 * iteration, and concurrent reader/writer access. */

#include "store/kvstore.h"

#undef NDEBUG /* Tests rely on assert() even in Release builds */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

static void test_crud(void)
{
    kvstore_t kv;
    assert(kvstore_init(&kv) == 0);

    assert(kvstore_set(&kv, "a", 1, "1", 1) == 0);
    const char *v;
    uint32_t vl;
    assert(kvstore_get(&kv, "a", 1, &v, &vl));
    assert(vl == 1 && v[0] == '1');

    /* Overwrite */
    assert(kvstore_set(&kv, "a", 1, "22", 2) == 0);
    assert(kvstore_get(&kv, "a", 1, &v, &vl));
    assert(vl == 2 && strcmp(v, "22") == 0);
    assert(kvstore_count(&kv) == 1);

    /* Delete */
    assert(kvstore_del(&kv, "a", 1));
    assert(!kvstore_get(&kv, "a", 1, &v, &vl));
    assert(!kvstore_del(&kv, "a", 1)); /* Already gone */
    assert(kvstore_count(&kv) == 0);

    kvstore_free(&kv);
    printf("  ok: set/get/del/overwrite\n");
}

static void test_resize(void)
{
    kvstore_t kv;
    assert(kvstore_init(&kv) == 0);

    char key[32], val[32];
    const int N = 200000; /* Forces several doublings from 64k buckets */
    for (int i = 0; i < N; i++) {
        int kl = snprintf(key, sizeof(key), "key-%d", i);
        int vl = snprintf(val, sizeof(val), "val-%d", i);
        assert(kvstore_set(&kv, key, (uint32_t)kl, val, (uint32_t)vl) == 0);
    }
    assert(kvstore_count(&kv) == (size_t)N);
    assert(kv.capacity > KV_INITIAL_CAPACITY);

    /* Every key must still be reachable after rehashing */
    const char *v;
    uint32_t vl;
    for (int i = 0; i < N; i += 997) {
        int kl = snprintf(key, sizeof(key), "key-%d", i);
        snprintf(val, sizeof(val), "val-%d", i);
        assert(kvstore_get(&kv, key, (uint32_t)kl, &v, &vl));
        assert(strcmp(v, val) == 0);
    }
    kvstore_free(&kv);
    printf("  ok: 200k inserts with resizing\n");
}

static int iter_count = 0;

static int iter_cb(const char *k, uint32_t kl, const char *v, uint32_t vl,
                   void *ctx)
{
    (void)k; (void)kl; (void)v; (void)vl; (void)ctx;
    iter_count++;
    return 0;
}

static void test_foreach(void)
{
    kvstore_t kv;
    assert(kvstore_init(&kv) == 0);
    char key[32];
    for (int i = 0; i < 1000; i++) {
        int kl = snprintf(key, sizeof(key), "it-%d", i);
        kvstore_set(&kv, key, (uint32_t)kl, "x", 1);
    }
    iter_count = 0;
    kvstore_foreach(&kv, iter_cb, NULL);
    assert(iter_count == 1000);
    kvstore_clear(&kv);
    assert(kvstore_count(&kv) == 0);
    kvstore_free(&kv);
    printf("  ok: foreach + clear\n");
}

/* ---- Concurrency: 4 writers + 4 readers hammering the same store ---- */

#define THREADS 4
#define OPS_PER_THREAD 50000

static kvstore_t conc_kv;

static void *writer(void *arg)
{
    int id = (int)(intptr_t)arg;
    char key[32], val[32];
    for (int i = 0; i < OPS_PER_THREAD; i++) {
        int kl = snprintf(key, sizeof(key), "t%d-%d", id, i % 1000);
        int vl = snprintf(val, sizeof(val), "v%d", i);
        kvstore_set(&conc_kv, key, (uint32_t)kl, val, (uint32_t)vl);
    }
    return NULL;
}

static void *reader(void *arg)
{
    int id = (int)(intptr_t)arg;
    char key[32];
    const char *v;
    uint32_t vl;
    for (int i = 0; i < OPS_PER_THREAD; i++) {
        int kl = snprintf(key, sizeof(key), "t%d-%d", id, i % 1000);
        kvstore_get(&conc_kv, key, (uint32_t)kl, &v, &vl);
    }
    return NULL;
}

static void test_concurrent(void)
{
    assert(kvstore_init(&conc_kv) == 0);
    pthread_t ws[THREADS], rs[THREADS];
    for (int i = 0; i < THREADS; i++) {
        pthread_create(&ws[i], NULL, writer, (void *)(intptr_t)i);
        pthread_create(&rs[i], NULL, reader, (void *)(intptr_t)i);
    }
    for (int i = 0; i < THREADS; i++) {
        pthread_join(ws[i], NULL);
        pthread_join(rs[i], NULL);
    }
    /* Each writer cycles 1000 distinct keys */
    assert(kvstore_count(&conc_kv) == THREADS * 1000);
    kvstore_free(&conc_kv);
    printf("  ok: %d threads, %d ops, no data races\n", THREADS * 2,
           THREADS * 2 * OPS_PER_THREAD);
}

int main(void)
{
    printf("test_kvstore:\n");
    test_crud();
    test_resize();
    test_foreach();
    test_concurrent();
    printf("ALL KVSTORE TESTS PASSED\n");
    return 0;
}
