/* Integration test: a 3-node Raft cluster simulated in-process with an
 * in-memory message bus (no sockets, no WAL). Verifies:
 *   1. Leader election
 *   2. Log replication + commit on all nodes
 *   3. Leader failure → re-election
 *   4. Writes with a 2/3 majority
 *   5. Failed node rejoining and catching up
 */

#include "raft/raft.h"
#include "common/log.h"

#undef NDEBUG /* Tests rely on assert() even in Release builds */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

#define N 3
#define QCAP 65536

typedef struct {
    int      to;
    uint8_t  type;
    buffer_t payload;
} msg_t;

static msg_t queue[QCAP];
static int qhead = 0, qtail = 0;

static raft_t nodes[N + 1];
static raft_log_t logs[N + 1];
static cluster_config_t cfgs[N + 1];
static int alive[N + 1] = {0, 1, 1, 1};
static int applied[N + 1] = {0};

static void send_cb(int node_id, uint8_t type, const buffer_t *payload,
                    void *ctx)
{
    (void)ctx;
    if (!alive[node_id] || (qtail - qhead) >= QCAP)
        return;
    msg_t *m = &queue[qtail++ % QCAP];
    m->to = node_id;
    m->type = type;
    buf_init(&m->payload, payload->len);
    buf_write_bytes(&m->payload, payload->data, payload->len);
}

static void apply_cb(const raft_entry_t *e, void *ctx)
{
    (void)e;
    applied[(int)(intptr_t)ctx]++;
}

static void deliver_all(void)
{
    while (qhead < qtail) {
        msg_t *m = &queue[qhead++ % QCAP];
        if (alive[m->to])
            raft_handle_message(&nodes[m->to], m->type, &m->payload);
        buf_free(&m->payload);
    }
}

static void tick_all(void)
{
    for (int i = 1; i <= N; i++)
        if (alive[i])
            raft_tick(&nodes[i]);
    deliver_all();
}

static int find_leader(void)
{
    for (int i = 1; i <= N; i++)
        if (alive[i] && nodes[i].role == RAFT_LEADER)
            return i;
    return 0;
}

static void run_ms(int ms)
{
    for (int t = 0; t < ms; t += 5) {
        tick_all();
        usleep(5000);
    }
}

int main(void)
{
    log_set_level(LOG_LEVEL_WARN);
    srand(12345);

    for (int i = 1; i <= N; i++) {
        config_defaults(&cfgs[i]);
        cfgs[i].node_id = i;
        snprintf(cfgs[i].data_dir, sizeof(cfgs[i].data_dir),
                 "/tmp/raft_test_node%d", i);
        mkdir(cfgs[i].data_dir, 0755);
        cfgs[i].num_peers = 0;
        for (int j = 1; j <= N; j++) {
            if (j == i)
                continue;
            cfgs[i].peers[cfgs[i].num_peers].node_id = j;
            cfgs[i].num_peers++;
        }
        raft_log_init(&logs[i], NULL); /* No WAL — pure state machine */
        raft_init(&nodes[i], &cfgs[i], &logs[i]);
        raft_set_send_cb(&nodes[i], send_cb, NULL);
        raft_set_apply_cb(&nodes[i], apply_cb, (void *)(intptr_t)i);
    }

    /* 1. A leader must emerge */
    run_ms(800);
    int leader = find_leader();
    assert(leader != 0);
    printf("  ok: node %d elected leader (term %llu)\n", leader,
           (unsigned long long)nodes[leader].current_term);

    /* 2. Replication: 10 commands reach every node */
    for (int i = 0; i < 10; i++) {
        char key[16];
        snprintf(key, sizeof(key), "k%d", i);
        assert(raft_submit(&nodes[leader], RAFT_CMD_SET, key,
                           (uint32_t)strlen(key), "v", 1) > 0);
    }
    run_ms(400);
    for (int i = 1; i <= N; i++)
        assert(applied[i] == 10);
    printf("  ok: 10 entries replicated and applied on all 3 nodes\n");

    /* 3. Kill the leader → new leader elected */
    alive[leader] = 0;
    run_ms(1500);
    int leader2 = find_leader();
    assert(leader2 != 0 && leader2 != leader);
    printf("  ok: new leader %d after killing %d\n", leader2, leader);

    /* 4. Writes still commit with 2/3 nodes */
    assert(raft_submit(&nodes[leader2], RAFT_CMD_SET, "after", 5, "x", 1) > 0);
    run_ms(400);
    for (int i = 1; i <= N; i++)
        if (alive[i])
            assert(applied[i] == 11);
    printf("  ok: writes commit with a 2/3 majority\n");

    /* 5. Old leader rejoins and catches up */
    alive[leader] = 1;
    run_ms(1000);
    assert(applied[leader] == 11);
    assert(nodes[leader].role == RAFT_FOLLOWER);
    printf("  ok: node %d rejoined and caught up (11 entries)\n", leader);

    for (int i = 1; i <= N; i++)
        raft_log_free(&logs[i]);
    printf("ALL RAFT TESTS PASSED\n");
    return 0;
}
