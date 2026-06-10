/* -----------------------------------------------------------------------
 * kvstore-cli — interactive client for the distributed KV store.
 *
 * Commands:
 *   SET <key> <value>   Write a key (replicated via Raft)
 *   GET <key>           Read a key (from the leader)
 *   DEL <key>           Delete a key
 *   STATUS              Show node status
 *   KEYS                Count of stored keys
 *   BENCH <n> [p]       Benchmark n SET ops with p pipelined requests
 *   HELP                Show help
 *   QUIT                Exit
 *
 * If the connected node is not the leader, the CLI automatically
 * reconnects to the leader (assumes client port = 6000 + node_id).
 * ----------------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include "common/buffer.h"

/* Protocol constants (mirrors raft_rpc.h / server main.c) */
#define MSG_CLIENT_REQUEST  0x10
#define MSG_CLIENT_RESPONSE 0x11

#define CLIENT_CMD_SET    1
#define CLIENT_CMD_DEL    2
#define CLIENT_CMD_GET    3
#define CLIENT_CMD_STATUS 4
#define CLIENT_CMD_KEYS   5

#define RESP_OK         0
#define RESP_NOT_FOUND  1
#define RESP_NOT_LEADER 2
#define RESP_ERROR      3

#define CLIENT_PORT_BASE 6000

static char g_host[64] = "127.0.0.1";
static int  g_port = 6001;
static int  g_fd = -1;

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ---- Socket helpers (blocking I/O — simple and adequate for a CLI) ---- */

static int cli_connect(void)
{
    if (g_fd >= 0)
        close(g_fd);

    g_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_fd < 0)
        return -1;

    int one = 1;
    setsockopt(g_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)g_port),
    };
    inet_pton(AF_INET, g_host, &addr.sin_addr);

    if (connect(g_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(g_fd);
        g_fd = -1;
        return -1;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, (const char *)buf + done, len - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, (char *)buf + done, len - done);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/* ---- Request / response ---- */

static int send_request(const buffer_t *payload)
{
    uint8_t header[5];
    header[0] = MSG_CLIENT_REQUEST;
    uint32_t len = (uint32_t)payload->len;
    header[1] = (uint8_t)(len >> 24); /* Network byte order */
    header[2] = (uint8_t)(len >> 16);
    header[3] = (uint8_t)(len >> 8);
    header[4] = (uint8_t)len;

    if (write_all(g_fd, header, 5) < 0)
        return -1;
    return write_all(g_fd, payload->data, payload->len);
}

/* Reads one response. Returns status code or -1 on socket error.
 * Value (if any) is copied into `value` (caller-sized). */
static int read_response(int32_t *leader_id, char *value, size_t value_cap,
                         uint32_t *value_len)
{
    uint8_t header[5];
    if (read_all(g_fd, header, 5) < 0)
        return -1;
    if (header[0] != MSG_CLIENT_RESPONSE)
        return -1;
    uint32_t len = ((uint32_t)header[1] << 24) | ((uint32_t)header[2] << 16) |
                   ((uint32_t)header[3] << 8) | (uint32_t)header[4];
    if (len < 9 || len > 16 * 1024 * 1024)
        return -1;

    uint8_t *payload = malloc(len);
    if (!payload || read_all(g_fd, payload, len) < 0) {
        free(payload);
        return -1;
    }

    uint8_t status = payload[0];
    int32_t leader = (int32_t)(((uint32_t)payload[1] << 24) |
                               ((uint32_t)payload[2] << 16) |
                               ((uint32_t)payload[3] << 8) |
                               (uint32_t)payload[4]);
    uint32_t vlen = ((uint32_t)payload[5] << 24) | ((uint32_t)payload[6] << 16) |
                    ((uint32_t)payload[7] << 8) | (uint32_t)payload[8];
    if (9 + vlen > len)
        vlen = len - 9;

    if (leader_id)
        *leader_id = leader;
    if (value && value_cap > 0) {
        uint32_t copy = vlen < value_cap - 1 ? vlen : (uint32_t)value_cap - 1;
        memcpy(value, payload + 9, copy);
        value[copy] = '\0';
        if (value_len)
            *value_len = copy;
    }
    free(payload);
    return status;
}

static void build_kv_request(buffer_t *b, uint8_t cmd, const char *key,
                             const char *value)
{
    buf_reset(b);
    buf_write_u8(b, cmd);
    if (key)
        buf_write_str(b, key);
    if (value) {
        uint32_t vl = (uint32_t)strlen(value);
        buf_write_u32(b, vl);
        buf_write_bytes(b, value, vl);
    }
}

/* Execute a request; on NOT_LEADER, hop to the leader and retry (max 3). */
static int execute(buffer_t *req, char *value, size_t value_cap)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        if (g_fd < 0 && cli_connect() < 0)
            return -1;
        if (send_request(req) < 0) {
            cli_connect();
            continue;
        }
        int32_t leader_id = -1;
        int status = read_response(&leader_id, value, value_cap, NULL);
        if (status < 0) {
            cli_connect();
            continue;
        }
        if (status == RESP_NOT_LEADER) {
            if (leader_id <= 0) {
                usleep(300 * 1000); /* Election in progress */
                continue;
            }
            g_port = CLIENT_PORT_BASE + leader_id;
            printf("(redirected to leader: node %d, port %d)\n", leader_id,
                   g_port);
            if (cli_connect() < 0)
                return -1;
            continue;
        }
        return status;
    }
    return -1;
}

/* ---- Benchmark ---- */

static int cmp_i64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void run_bench(int n, int pipeline)
{
    if (n <= 0)
        return;
    if (pipeline < 1)
        pipeline = 1;
    if (pipeline > 256)
        pipeline = 256;

    /* Warm-up write through execute(): if we're connected to a follower,
     * this redirects us to the leader before the timed run starts. */
    {
        buffer_t warm;
        buf_init(&warm, 64);
        build_kv_request(&warm, CLIENT_CMD_SET, "bench-warmup", "x");
        int st = execute(&warm, NULL, 0);
        buf_free(&warm);
        if (st != RESP_OK) {
            printf("Cannot reach the leader (status %d) — is the cluster "
                   "up?\n", st);
            return;
        }
    }

    printf("Benchmark: %d SET ops, pipeline depth %d...\n", n, pipeline);

    /* Latency samples for percentiles (per batch) */
    int64_t *lat = malloc(sizeof(int64_t) * (size_t)((n + pipeline - 1) /
                                                     pipeline));
    int batches = 0;

    buffer_t req;
    buf_init(&req, 256);
    char key[64], val[64];

    int64_t t0 = now_us();
    int sent = 0, acked = 0;

    while (acked < n) {
        int burst = pipeline;
        if (sent + burst > n)
            burst = n - sent;

        int64_t b0 = now_us();
        for (int i = 0; i < burst; i++) {
            snprintf(key, sizeof(key), "bench-%d", sent + i);
            snprintf(val, sizeof(val), "value-%d", sent + i);
            build_kv_request(&req, CLIENT_CMD_SET, key, val);
            if (send_request(&req) < 0) {
                printf("send failed\n");
                goto out;
            }
        }
        sent += burst;

        for (int i = 0; i < burst; i++) {
            int status = read_response(NULL, NULL, 0, NULL);
            if (status != RESP_OK) {
                printf("op failed (status %d) — is this the leader?\n",
                       status);
                goto out;
            }
            acked++;
        }
        lat[batches++] = (now_us() - b0) / burst;

        if (acked % 10000 < pipeline)
            printf("  ... %d/%d\n", acked, n);
    }

out:;
    int64_t elapsed = now_us() - t0;
    if (acked > 0 && elapsed > 0) {
        double secs = (double)elapsed / 1e6;
        double ops = (double)acked / secs;
        qsort(lat, (size_t)batches, sizeof(int64_t), cmp_i64);
        printf("\nResults:\n");
        printf("  %d ops in %.2fs = %.0f ops/sec\n", acked, secs, ops);
        if (batches > 0) {
            printf("  latency/op: p50=%lldus p95=%lldus p99=%lldus\n",
                   (long long)lat[batches / 2],
                   (long long)lat[(int)((double)batches * 0.95)],
                   (long long)lat[(int)((double)batches * 0.99)]);
        }
    }
    free(lat);
    buf_free(&req);
}

/* ---- Main loop ---- */

static void print_help(void)
{
    printf("Commands:\n"
           "  SET <key> <value>   Write a key\n"
           "  GET <key>           Read a key\n"
           "  DEL <key>           Delete a key\n"
           "  STATUS              Node status\n"
           "  KEYS                Number of keys\n"
           "  BENCH <n> [p]       Benchmark n writes (pipeline p, default 16)\n"
           "  HELP                This help\n"
           "  QUIT                Exit\n");
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--host") == 0)
            snprintf(g_host, sizeof(g_host), "%s", argv[i + 1]);
        else if (strcmp(argv[i], "--port") == 0)
            g_port = atoi(argv[i + 1]);
    }

    if (cli_connect() < 0) {
        fprintf(stderr, "Cannot connect to %s:%d: %s\n", g_host, g_port,
                strerror(errno));
        return 1;
    }
    printf("Connected to %s:%d. Type HELP for commands.\n", g_host, g_port);

    buffer_t req;
    buf_init(&req, 256);
    char line[8192], value[65536];

    for (;;) {
        printf("kvstore> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin))
            break;

        char *cmd = strtok(line, " \t\r\n");
        if (!cmd)
            continue;

        if (strcasecmp(cmd, "QUIT") == 0 || strcasecmp(cmd, "EXIT") == 0)
            break;

        if (strcasecmp(cmd, "HELP") == 0) {
            print_help();
            continue;
        }

        if (strcasecmp(cmd, "SET") == 0) {
            char *key = strtok(NULL, " \t\r\n");
            char *val = strtok(NULL, "\r\n"); /* Rest of line = value */
            if (!key || !val) {
                printf("usage: SET <key> <value>\n");
                continue;
            }
            build_kv_request(&req, CLIENT_CMD_SET, key, val);
            int64_t t0 = now_us();
            int st = execute(&req, NULL, 0);
            if (st == RESP_OK)
                printf("OK (%.1fms)\n", (double)(now_us() - t0) / 1000.0);
            else
                printf("ERROR (status %d)\n", st);
            continue;
        }

        if (strcasecmp(cmd, "GET") == 0) {
            char *key = strtok(NULL, " \t\r\n");
            if (!key) {
                printf("usage: GET <key>\n");
                continue;
            }
            build_kv_request(&req, CLIENT_CMD_GET, key, NULL);
            int64_t t0 = now_us();
            int st = execute(&req, value, sizeof(value));
            double ms = (double)(now_us() - t0) / 1000.0;
            if (st == RESP_OK)
                printf("\"%s\" (%.1fms)\n", value, ms);
            else if (st == RESP_NOT_FOUND)
                printf("(not found) (%.1fms)\n", ms);
            else
                printf("ERROR (status %d)\n", st);
            continue;
        }

        if (strcasecmp(cmd, "DEL") == 0) {
            char *key = strtok(NULL, " \t\r\n");
            if (!key) {
                printf("usage: DEL <key>\n");
                continue;
            }
            build_kv_request(&req, CLIENT_CMD_DEL, key, NULL);
            int64_t t0 = now_us();
            int st = execute(&req, NULL, 0);
            if (st == RESP_OK)
                printf("OK (%.1fms)\n", (double)(now_us() - t0) / 1000.0);
            else
                printf("ERROR (status %d)\n", st);
            continue;
        }

        if (strcasecmp(cmd, "STATUS") == 0) {
            build_kv_request(&req, CLIENT_CMD_STATUS, NULL, NULL);
            if (execute(&req, value, sizeof(value)) == RESP_OK)
                printf("%s\n", value);
            else
                printf("ERROR\n");
            continue;
        }

        if (strcasecmp(cmd, "KEYS") == 0) {
            build_kv_request(&req, CLIENT_CMD_KEYS, NULL, NULL);
            if (execute(&req, value, sizeof(value)) == RESP_OK)
                printf("%s keys stored\n", value);
            else
                printf("ERROR\n");
            continue;
        }

        if (strcasecmp(cmd, "BENCH") == 0) {
            char *ns = strtok(NULL, " \t\r\n");
            char *ps = strtok(NULL, " \t\r\n");
            int n = ns ? atoi(ns) : 10000;
            int p = ps ? atoi(ps) : 16;
            run_bench(n, p);
            continue;
        }

        printf("Unknown command '%s'. Type HELP.\n", cmd);
    }

    buf_free(&req);
    if (g_fd >= 0)
        close(g_fd);
    printf("Bye.\n");
    return 0;
}
