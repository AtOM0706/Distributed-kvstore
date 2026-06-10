/* Unit tests: WAL append/replay/truncate/compaction + corruption recovery,
 * and snapshot write/load round-trip. */

#include "wal/wal.h"
#include "wal/snapshot.h"

#undef NDEBUG /* Tests rely on assert() even in Release builds */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#define TEST_WAL  "/tmp/kvstore_test.wal"
#define TEST_SNAP "/tmp/kvstore_test.snap"

static int replay_count = 0;

static int replay_cb(uint64_t term, uint64_t index, uint8_t cmd,
                     const uint8_t *p, uint32_t len, void *ctx)
{
    (void)term; (void)cmd; (void)p; (void)ctx;
    assert(index == (uint64_t)replay_count + 1);
    assert(len == 5);
    replay_count++;
    return 0;
}

static int snap_count = 0;

static int snap_cb(const char *k, uint32_t kl, const char *v, uint32_t vl,
                   void *c)
{
    (void)c;
    assert(kl == 4 && vl == 6);
    assert(strncmp(k, "key", 3) == 0 && strncmp(v, "value", 5) == 0);
    snap_count++;
    return 0;
}

static void test_append_replay(void)
{
    unlink(TEST_WAL);
    wal_t w;
    assert(wal_open(&w, TEST_WAL) == 0);
    for (int i = 1; i <= 100; i++)
        assert(wal_append(&w, 1, (uint64_t)i, WAL_CMD_SET, "hello", 5) == 0);
    assert(wal_sync(&w) == 0);
    assert(w.last_index == 100);
    assert(w.entry_count == 100);
    wal_close(&w);

    assert(wal_open(&w, TEST_WAL) == 0);
    assert(w.entry_count == 100);
    replay_count = 0;
    assert(wal_replay(&w, replay_cb, NULL) == 100);
    assert(replay_count == 100);
    wal_close(&w);
    printf("  ok: append + replay (100 entries)\n");
}

static void test_truncate(void)
{
    wal_t w;
    assert(wal_open(&w, TEST_WAL) == 0);

    assert(wal_truncate_after(&w, 50) == 0);
    assert(w.last_index == 50 && w.entry_count == 50);

    assert(wal_truncate_before(&w, 20) == 0);
    assert(w.entry_count == 30);
    assert(w.last_index == 50);
    wal_close(&w);
    printf("  ok: truncate_after + truncate_before\n");
}

static void test_corruption(void)
{
    /* Flip a byte mid-file: replay must stop at the corrupt entry and
     * trim the rest. */
    FILE *f = fopen(TEST_WAL, "r+b");
    assert(f);
    fseek(f, 500, SEEK_SET);
    fputc(0xFF, f);
    fclose(f);

    wal_t w;
    assert(wal_open(&w, TEST_WAL) == 0);
    assert(w.entry_count < 30); /* Some entries were trimmed */
    wal_close(&w);
    printf("  ok: corruption detected and trimmed\n");
}

static void test_snapshot(void)
{
    unlink(TEST_SNAP);
    snapshot_writer_t *sw = snapshot_begin(TEST_SNAP, 42, 7);
    assert(sw);
    assert(snapshot_add(sw, "key1", 4, "value1", 6) == 0);
    assert(snapshot_add(sw, "key2", 4, "value2", 6) == 0);
    assert(snapshot_commit(sw) == 0);

    uint64_t li, lt;
    snap_count = 0;
    assert(snapshot_load(TEST_SNAP, snap_cb, NULL, &li, &lt) == 2);
    assert(li == 42 && lt == 7 && snap_count == 2);

    /* Missing snapshot is not an error */
    assert(snapshot_load("/tmp/kvstore_nope.snap", snap_cb, NULL, &li,
                         &lt) == 0);
    assert(li == 0);
    printf("  ok: snapshot write + load round-trip\n");
}

int main(void)
{
    printf("test_wal:\n");
    test_append_replay();
    test_truncate();
    test_corruption();
    test_snapshot();
    unlink(TEST_WAL);
    unlink(TEST_SNAP);
    printf("ALL WAL TESTS PASSED\n");
    return 0;
}
