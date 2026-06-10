#include "raft_timer.h"

#include <stdlib.h>
#include <time.h>

int64_t time_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int64_t random_timeout(const raft_timer_t *t)
{
    int span = t->election_max_ms - t->election_min_ms;
    int jitter = span > 0 ? rand() % span : 0;
    return t->election_min_ms + jitter;
}

void timer_init(raft_timer_t *t, int election_min_ms, int election_max_ms,
                int heartbeat_ms)
{
    t->election_min_ms = election_min_ms;
    t->election_max_ms = election_max_ms;
    t->heartbeat_ms = heartbeat_ms;
    timer_reset_election(t);
    t->next_heartbeat = time_now_ms() + heartbeat_ms;
}

void timer_reset_election(raft_timer_t *t)
{
    t->election_deadline = time_now_ms() + random_timeout(t);
}

bool timer_election_expired(const raft_timer_t *t)
{
    return time_now_ms() >= t->election_deadline;
}

bool timer_heartbeat_due(raft_timer_t *t)
{
    int64_t now = time_now_ms();
    if (now >= t->next_heartbeat) {
        t->next_heartbeat = now + t->heartbeat_ms;
        return true;
    }
    return false;
}
