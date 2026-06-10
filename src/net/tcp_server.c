#include "tcp_server.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define READ_CHUNK 65536

/* A listener registration, stored as the epoll data pointer. We
 * distinguish listeners from connections by the `is_listener` tag that
 * both structs start with. */
typedef struct {
    int            is_listener; /* Always 1 */
    int            fd;
    net_accept_cb  on_accept;
    void          *ud;
    net_loop_t    *loop;
} net_listener_t;

/* net_conn_t cannot start with is_listener without breaking the public
 * struct, so we wrap every epoll registration in a small tagged box. */
typedef struct {
    int   is_listener;
    void *ptr; /* net_listener_t* or net_conn_t* */
} epoll_tag_t;

int net_set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int net_loop_init(net_loop_t *loop)
{
    memset(loop, 0, sizeof(*loop));
    loop->epfd = epoll_create1(0);
    if (loop->epfd < 0) {
        LOG_ERROR("net", "epoll_create1: %s", strerror(errno));
        return -1;
    }
    loop->running = true;
    return 0;
}

void net_loop_shutdown(net_loop_t *loop)
{
    while (loop->conns)
        net_conn_destroy(loop->conns);
    if (loop->epfd >= 0)
        close(loop->epfd);
    loop->epfd = -1;
    loop->running = false;
}

int net_loop_listen(net_loop_t *loop, int port, net_accept_cb on_accept,
                    void *ud)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons((uint16_t)port),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("net", "bind(:%d): %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 128) < 0 || net_set_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }

    net_listener_t *l = calloc(1, sizeof(*l));
    epoll_tag_t *tag = calloc(1, sizeof(*tag));
    if (!l || !tag) {
        free(l);
        free(tag);
        close(fd);
        return -1;
    }
    l->is_listener = 1;
    l->fd = fd;
    l->on_accept = on_accept;
    l->ud = ud;
    l->loop = loop;
    tag->is_listener = 1;
    tag->ptr = l;

    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = tag };
    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        free(l);
        free(tag);
        close(fd);
        return -1;
    }

    LOG_INFO("net", "Listening on port %d", port);
    return fd;
}

static void conn_update_events(net_conn_t *c)
{
    struct epoll_event ev = {
        .events = EPOLLIN | (c->out.len > c->out.read_pos ? EPOLLOUT : 0),
        .data.ptr = c->loop_tag_storage,
    };
    epoll_ctl(c->loop->epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

net_conn_t *net_conn_register(net_loop_t *loop, int fd)
{
    net_conn_t *c = calloc(1, sizeof(*c));
    epoll_tag_t *tag = calloc(1, sizeof(*tag));
    if (!c || !tag) {
        free(c);
        free(tag);
        return NULL;
    }

    net_set_nonblocking(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    c->fd = fd;
    c->loop = loop;
    c->peer_id = 0;
    buf_init(&c->in, 4096);
    buf_init(&c->out, 4096);
    tag->is_listener = 0;
    tag->ptr = c;
    c->loop_tag_storage = tag;

    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = tag };
    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        buf_free(&c->in);
        buf_free(&c->out);
        free(tag);
        free(c);
        return NULL;
    }

    /* Link into the connection list */
    c->next = loop->conns;
    if (loop->conns)
        loop->conns->prev = c;
    loop->conns = c;
    loop->num_conns++;
    return c;
}

void net_conn_destroy(net_conn_t *c)
{
    net_loop_t *loop = c->loop;

    if (c->on_close)
        c->on_close(c);

    epoll_ctl(loop->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);

    if (c->prev)
        c->prev->next = c->next;
    else
        loop->conns = c->next;
    if (c->next)
        c->next->prev = c->prev;
    loop->num_conns--;

    buf_free(&c->in);
    buf_free(&c->out);
    free(c->loop_tag_storage);
    free(c);
}

void net_conn_close(net_conn_t *c)
{
    if (c->out.len > c->out.read_pos) {
        c->closing = true; /* Flush first */
        conn_update_events(c);
    } else {
        net_conn_destroy(c);
    }
}

/* Flush as much of the out buffer as the socket accepts.
 * Returns -1 if the connection died. */
static int conn_flush(net_conn_t *c)
{
    while (c->out.read_pos < c->out.len) {
        ssize_t n = write(c->fd, c->out.data + c->out.read_pos,
                          c->out.len - c->out.read_pos);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            return -1;
        }
        c->out.read_pos += (size_t)n;
    }

    /* Compact the out buffer */
    if (c->out.read_pos == c->out.len)
        buf_reset(&c->out);

    if (c->closing && c->out.len == 0) {
        net_conn_destroy(c);
        return -1; /* Caller must not touch c */
    }
    conn_update_events(c);
    return 0;
}

int net_conn_send(net_conn_t *c, const void *data, size_t len)
{
    if (c->closing)
        return -1;
    buf_write_bytes(&c->out, data, len);
    return conn_flush(c) == 0 ? 0 : -1;
}

static void handle_readable(net_conn_t *c)
{
    uint8_t chunk[READ_CHUNK];
    bool got_data = false;

    for (;;) {
        ssize_t n = read(c->fd, chunk, sizeof(chunk));
        if (n > 0) {
            buf_write_bytes(&c->in, chunk, (size_t)n);
            got_data = true;
            if ((size_t)n < sizeof(chunk))
                break;
        } else if (n == 0) {
            net_conn_destroy(c); /* Peer closed */
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            net_conn_destroy(c);
            return;
        }
    }

    if (got_data && c->on_data)
        c->on_data(c);
}

static void handle_accept(net_listener_t *l)
{
    for (;;) {
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        int fd = accept(l->fd, (struct sockaddr *)&addr, &alen);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            if (errno == EINTR)
                continue;
            LOG_WARN("net", "accept: %s", strerror(errno));
            return;
        }
        net_conn_t *c = net_conn_register(l->loop, fd);
        if (!c) {
            close(fd);
            continue;
        }
        if (l->on_accept)
            l->on_accept(c, l->ud);
    }
}

int net_loop_poll(net_loop_t *loop, int timeout_ms)
{
    struct epoll_event events[NET_MAX_EVENTS];
    int n = epoll_wait(loop->epfd, events, NET_MAX_EVENTS, timeout_ms);
    if (n < 0) {
        if (errno == EINTR)
            return 0;
        LOG_ERROR("net", "epoll_wait: %s", strerror(errno));
        return -1;
    }

    for (int i = 0; i < n; i++) {
        epoll_tag_t *tag = events[i].data.ptr;

        if (tag->is_listener) {
            handle_accept((net_listener_t *)tag->ptr);
            continue;
        }

        net_conn_t *c = tag->ptr;
        uint32_t evs = events[i].events;

        if (evs & (EPOLLERR | EPOLLHUP)) {
            net_conn_destroy(c);
            continue;
        }
        if (evs & EPOLLOUT) {
            if (conn_flush(c) < 0)
                continue; /* Destroyed */
        }
        if (evs & EPOLLIN)
            handle_readable(c);
    }
    return n;
}
