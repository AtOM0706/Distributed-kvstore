#include "common/config.h"
#include "common/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <sys/stat.h>
#include <errno.h>

/* -----------------------------------------------------------------------
 * Command-line argument parser for cluster configuration.
 * ----------------------------------------------------------------------- */

void config_defaults(cluster_config_t *config) {
    memset(config, 0, sizeof(*config));
    config->node_id               = -1;
    config->client_port           = 0;
    config->raft_port             = 0;
    config->ws_port               = 0;
    config->num_peers             = 0;
    config->election_timeout_min_ms = 150;
    config->election_timeout_max_ms = 300;
    config->heartbeat_interval_ms   = 50;
    config->snapshot_threshold      = 10000;
    config->max_key_size            = 256;
    config->max_value_size          = 1048576; /* 1 MB */
    snprintf(config->data_dir, sizeof(config->data_dir), "./data");
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "\n"
        "Required:\n"
        "  --id <N>              Node ID (1, 2, 3, ...)\n"
        "  --client-port <PORT>  Port for client connections\n"
        "  --raft-port <PORT>    Port for Raft peer communication\n"
        "  --ws-port <PORT>      Port for WebSocket dashboard\n"
        "  --peer <HOST:PORT>    Peer address (can be repeated)\n"
        "\n"
        "Optional:\n"
        "  --data-dir <PATH>     Data directory (default: ./data/node-<id>)\n"
        "  --election-min <MS>   Min election timeout (default: 150)\n"
        "  --election-max <MS>   Max election timeout (default: 300)\n"
        "  --heartbeat <MS>      Heartbeat interval (default: 50)\n"
        "  --snapshot-thresh <N> Snapshot threshold entries (default: 10000)\n"
        "  --log-level <LEVEL>   Log level: trace,debug,info,warn,error (default: info)\n"
        "  --help                Show this help\n"
        "\n"
        "Example:\n"
        "  %s --id 1 --client-port 6001 --raft-port 7001 --ws-port 8001 \\\n"
        "      --peer 127.0.0.1:7002 --peer 127.0.0.1:7003\n",
        prog, prog);
}

static int parse_peer(const char *arg, peer_config_t *peer) {
    /* Format: HOST:PORT */
    const char *colon = strrchr(arg, ':');
    if (!colon || colon == arg) {
        fprintf(stderr, "Invalid peer format '%s' (expected HOST:PORT)\n", arg);
        return -1;
    }

    size_t host_len = (size_t)(colon - arg);
    if (host_len >= MAX_HOST_LEN) {
        fprintf(stderr, "Peer host too long: '%s'\n", arg);
        return -1;
    }

    memcpy(peer->host, arg, host_len);
    peer->host[host_len] = '\0';
    peer->raft_port = atoi(colon + 1);

    if (peer->raft_port <= 0 || peer->raft_port > 65535) {
        fprintf(stderr, "Invalid peer port: '%s'\n", colon + 1);
        return -1;
    }

    return 0;
}

static int mkdirs(const char *path) {
    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

int config_parse(cluster_config_t *config, int argc, char **argv) {
    config_defaults(config);

    static struct option long_opts[] = {
        {"id",              required_argument, NULL, 'i'},
        {"client-port",     required_argument, NULL, 'c'},
        {"raft-port",       required_argument, NULL, 'r'},
        {"ws-port",         required_argument, NULL, 'w'},
        {"peer",            required_argument, NULL, 'p'},
        {"data-dir",        required_argument, NULL, 'd'},
        {"election-min",    required_argument, NULL, 'E'},
        {"election-max",    required_argument, NULL, 'X'},
        {"heartbeat",       required_argument, NULL, 'H'},
        {"snapshot-thresh", required_argument, NULL, 'S'},
        {"log-level",       required_argument, NULL, 'l'},
        {"help",            no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    int custom_data_dir = 0;

    while ((opt = getopt_long(argc, argv, "i:c:r:w:p:d:l:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'i':
            config->node_id = atoi(optarg);
            break;
        case 'c':
            config->client_port = atoi(optarg);
            break;
        case 'r':
            config->raft_port = atoi(optarg);
            break;
        case 'w':
            config->ws_port = atoi(optarg);
            break;
        case 'p':
            if (config->num_peers >= MAX_PEERS) {
                fprintf(stderr, "Too many peers (max %d)\n", MAX_PEERS);
                return -1;
            }
            if (parse_peer(optarg, &config->peers[config->num_peers]) != 0)
                return -1;
            config->num_peers++;
            break;
        case 'd':
            snprintf(config->data_dir, sizeof(config->data_dir), "%s", optarg);
            custom_data_dir = 1;
            break;
        case 'E':
            config->election_timeout_min_ms = atoi(optarg);
            break;
        case 'X':
            config->election_timeout_max_ms = atoi(optarg);
            break;
        case 'H':
            config->heartbeat_interval_ms = atoi(optarg);
            break;
        case 'S':
            config->snapshot_threshold = atoi(optarg);
            break;
        case 'l':
            if (strcmp(optarg, "trace") == 0)      log_set_level(LOG_LEVEL_TRACE);
            else if (strcmp(optarg, "debug") == 0)  log_set_level(LOG_LEVEL_DEBUG);
            else if (strcmp(optarg, "info") == 0)   log_set_level(LOG_LEVEL_INFO);
            else if (strcmp(optarg, "warn") == 0)   log_set_level(LOG_LEVEL_WARN);
            else if (strcmp(optarg, "error") == 0)  log_set_level(LOG_LEVEL_ERROR);
            else {
                fprintf(stderr, "Invalid log level: %s\n", optarg);
                return -1;
            }
            break;
        case 'h':
            print_usage(argv[0]);
            exit(0);
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    /* Validate required fields */
    if (config->node_id < 1) {
        fprintf(stderr, "Error: --id is required and must be >= 1\n");
        print_usage(argv[0]);
        return -1;
    }
    if (config->client_port <= 0) {
        config->client_port = 6000 + config->node_id;
    }
    if (config->raft_port <= 0) {
        config->raft_port = 7000 + config->node_id;
    }
    if (config->ws_port <= 0) {
        config->ws_port = 8000 + config->node_id;
    }

    /* Set data directory if not custom */
    if (!custom_data_dir) {
        snprintf(config->data_dir, sizeof(config->data_dir),
                 "./data/node-%d", config->node_id);
    }

    /* Create data directory */
    if (mkdirs(config->data_dir) != 0) {
        fprintf(stderr, "Failed to create data directory '%s': %s\n",
                config->data_dir, strerror(errno));
        return -1;
    }

    /* Assign peer node IDs based on their Raft port convention */
    for (int i = 0; i < config->num_peers; i++) {
        /* Try to derive node_id from raft_port (7001→1, 7002→2, etc.) */
        int port = config->peers[i].raft_port;
        if (port >= 7001 && port <= 7000 + MAX_NODES) {
            config->peers[i].node_id = port - 7000;
        } else {
            config->peers[i].node_id = i + 1; /* Fallback */
            if (config->peers[i].node_id >= config->node_id)
                config->peers[i].node_id++;
        }
    }

    return 0;
}

void config_print(const cluster_config_t *config) {
    LOG_INFO("config", "Node ID:       %d", config->node_id);
    LOG_INFO("config", "Client port:   %d", config->client_port);
    LOG_INFO("config", "Raft port:     %d", config->raft_port);
    LOG_INFO("config", "WebSocket port: %d", config->ws_port);
    LOG_INFO("config", "Data dir:      %s", config->data_dir);
    LOG_INFO("config", "Election timeout: %d-%dms",
             config->election_timeout_min_ms, config->election_timeout_max_ms);
    LOG_INFO("config", "Heartbeat:     %dms", config->heartbeat_interval_ms);

    for (int i = 0; i < config->num_peers; i++) {
        LOG_INFO("config", "Peer %d:        %s:%d (node %d)",
                 i + 1, config->peers[i].host, config->peers[i].raft_port,
                 config->peers[i].node_id);
    }
}
