#ifndef KVSTORE_CONFIG_H
#define KVSTORE_CONFIG_H

#include <stdint.h>

/* -----------------------------------------------------------------------
 * Cluster configuration — parsed from command-line arguments.
 *
 * Usage:
 *   ./kvstore-server --id 1 --client-port 6001 --raft-port 7001 \
 *       --ws-port 8001 --peer 127.0.0.1:7002 --peer 127.0.0.1:7003 \
 *       --data-dir ./data/node-1
 * ----------------------------------------------------------------------- */

#define MAX_NODES       8
#define MAX_PEERS       (MAX_NODES - 1)
#define MAX_HOST_LEN    64
#define MAX_PATH_LEN    256

typedef struct {
    char host[MAX_HOST_LEN];
    int  raft_port;
    int  node_id;   /* Peer's node ID (derived from port or explicitly set) */
} peer_config_t;

typedef struct {
    int             node_id;                /* This node's ID (1, 2, 3, ...) */
    int             client_port;            /* Port for client connections */
    int             raft_port;              /* Port for Raft peer communication */
    int             ws_port;                /* Port for WebSocket dashboard */
    char            data_dir[MAX_PATH_LEN]; /* Directory for WAL + snapshots */
    int             num_peers;              /* Number of peers in the cluster */
    peer_config_t   peers[MAX_PEERS];       /* Peer connection info */

    /* Tuning parameters */
    int             election_timeout_min_ms;  /* Default: 150 */
    int             election_timeout_max_ms;  /* Default: 300 */
    int             heartbeat_interval_ms;    /* Default: 50 */
    int             snapshot_threshold;        /* Entries before compaction. Default: 10000 */
    int             max_key_size;             /* Default: 256 */
    int             max_value_size;           /* Default: 1048576 (1 MB) */
} cluster_config_t;

/* Parse command-line arguments into a config struct.
 * Returns 0 on success, -1 on error (prints usage). */
int config_parse(cluster_config_t *config, int argc, char **argv);

/* Print the configuration (for debugging) */
void config_print(const cluster_config_t *config);

/* Set sensible defaults */
void config_defaults(cluster_config_t *config);

#endif /* KVSTORE_CONFIG_H */
