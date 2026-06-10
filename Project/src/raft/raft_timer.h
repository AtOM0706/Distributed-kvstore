#ifndef KVSTORE_RAFT_TIMER_H
#define KVSTORE_RAFT_TIMER_H

#include <stdint.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * Raft timers — election timeout (randomized) and heartbeat interval.
 * All times are milliseconds from CLOCK_MONOTONIC, immune to wall-clock
 * jumps (NTP, DST, manual changes).
 * ----------------------------------------------------------------------- */

typedef struct {
    int     election_min_ms;     /* e.g. 150 */
    int     election_max_ms;     /* e.g. 300 */
    int     heartbeat_ms;        /* e.g. 50  */
    int64_t election_deadline;   /* Absolute monotonic ms */
    int64_t next_heartbeat;      /* Absolute monotonic ms */
} raft_timer_t;

/* Current monotonic time in milliseconds */
int64_t time_now_ms(void);

/* Initialize with the given bounds and arm the election timer */
void timer_init(raft_timer_t *t, int election_min_ms, int election_max_ms,
                int heartbeat_ms);

/* Re-arm the election timer with a fresh random timeout.
 * Called when: a valid heartbeat arrives, we vote, or we start an election. */
void timer_reset_election(raft_timer_t *t);

/* Has the election timeout expired? */
bool timer_election_expired(const raft_timer_t *t);

/* Is it time for the leader to send heartbeats? (Also re-arms.) */
bool timer_heartbeat_due(raft_timer_t *t);

#endif /* KVSTORE_RAFT_TIMER_H */
