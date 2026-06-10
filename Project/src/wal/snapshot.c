#include "snapshot.h"
#include "common/log.h"
#include "common/hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define SNAP_HEADER_SIZE 32 /* magic(4) version(4) index(8) term(8) count(8) */

struct snapshot_writer {
    int      fd;
    char     final_path[512];
    char     tmp_path[600];
    uint64_t num_entries;
    uint32_t crc;       /* Running CRC over everything after magic */
    uint64_t last_included_index;
    uint64_t last_included_term;
};

/* Running CRC32 (matches crc32() in common/hash.c, which is one-shot —
 * we keep a rolling state by re-implementing the update step here). */
static uint32_t crc32_update(uint32_t crc, const void *data, size_t len);

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static int write_exact(int fd, const void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, (const uint8_t *)buf + done, len - done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static int read_exact(int fd, void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, (uint8_t *)buf + done, len - done);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Rolling CRC32 (same polynomial/reflection as common/hash.c crc32()).
 * ----------------------------------------------------------------------- */
static uint32_t snap_crc_table[256];
static int snap_crc_init = 0;

static void snap_crc_build(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        snap_crc_table[i] = c;
    }
    snap_crc_init = 1;
}

static uint32_t crc32_update(uint32_t crc, const void *data, size_t len)
{
    if (!snap_crc_init)
        snap_crc_build();
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++)
        crc = snap_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* Chain two CRC computations: state is kept un-finalized between calls by
 * storing the finalized value and re-inverting. (crc32_update finalizes
 * each call, so feeding it the previous result chains correctly only if
 * we treat the previous result as the seed.) */
static uint32_t crc_chain(uint32_t prev, const void *data, size_t len)
{
    /* Standard trick: crc32 with initial value = prev (already final). */
    if (!snap_crc_init)
        snap_crc_build();
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = ~prev;
    for (size_t i = 0; i < len; i++)
        crc = snap_crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* -----------------------------------------------------------------------
 * Writer
 * ----------------------------------------------------------------------- */

snapshot_writer_t *snapshot_begin(const char *path,
                                  uint64_t last_included_index,
                                  uint64_t last_included_term)
{
    snapshot_writer_t *w = calloc(1, sizeof(*w));
    if (!w)
        return NULL;

    snprintf(w->final_path, sizeof(w->final_path), "%s", path);
    snprintf(w->tmp_path, sizeof(w->tmp_path), "%s.tmp", path);
    w->last_included_index = last_included_index;
    w->last_included_term = last_included_term;

    w->fd = open(w->tmp_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (w->fd < 0) {
        LOG_ERROR("snapshot", "open(%s) failed: %s", w->tmp_path,
                  strerror(errno));
        free(w);
        return NULL;
    }

    /* Write header with placeholder count (patched in commit). */
    uint8_t hdr[SNAP_HEADER_SIZE];
    put_u32(hdr, SNAPSHOT_MAGIC);
    put_u32(hdr + 4, SNAPSHOT_VERSION);
    put_u64(hdr + 8, last_included_index);
    put_u64(hdr + 16, last_included_term);
    put_u64(hdr + 24, 0);

    if (write_exact(w->fd, hdr, sizeof(hdr)) < 0) {
        close(w->fd);
        unlink(w->tmp_path);
        free(w);
        return NULL;
    }
    return w;
}

int snapshot_add(snapshot_writer_t *w, const char *key, uint32_t key_len,
                 const char *value, uint32_t value_len)
{
    uint8_t len_buf[4];

    put_u32(len_buf, key_len);
    if (write_exact(w->fd, len_buf, 4) < 0 ||
        write_exact(w->fd, key, key_len) < 0)
        return -1;

    put_u32(len_buf, value_len);
    if (write_exact(w->fd, len_buf, 4) < 0 ||
        write_exact(w->fd, value, value_len) < 0)
        return -1;

    w->num_entries++;
    return 0;
}

int snapshot_commit(snapshot_writer_t *w)
{
    /* Patch the entry count in the header. */
    uint8_t cnt[8];
    put_u64(cnt, w->num_entries);
    if (pwrite(w->fd, cnt, 8, 24) != 8)
        goto fail;

    /* CRC covers: version..count fields + all entry bytes. The count was
     * only just finalized, so compute the whole thing in one streaming
     * pass: header body first, then the entries re-read from the file. */
    uint8_t hdr_body[SNAP_HEADER_SIZE - 4];
    put_u32(hdr_body, SNAPSHOT_VERSION);
    put_u64(hdr_body + 4, w->last_included_index);
    put_u64(hdr_body + 12, w->last_included_term);
    put_u64(hdr_body + 20, w->num_entries);
    uint32_t final_crc = crc32_update(0, hdr_body, sizeof(hdr_body));
    {
        off_t end = lseek(w->fd, 0, SEEK_END);
        if (end < 0)
            goto fail;
        uint64_t entries_len = (uint64_t)end - SNAP_HEADER_SIZE;
        uint8_t buf[65536];
        uint64_t off = SNAP_HEADER_SIZE;
        while (entries_len > 0) {
            size_t chunk = entries_len < sizeof(buf) ? (size_t)entries_len
                                                     : sizeof(buf);
            ssize_t n = pread(w->fd, buf, chunk, (off_t)off);
            if (n <= 0)
                goto fail;
            final_crc = crc_chain(final_crc, buf, (size_t)n);
            off += (uint64_t)n;
            entries_len -= (uint64_t)n;
        }
    }

    uint8_t crc_buf[4];
    put_u32(crc_buf, final_crc);
    if (lseek(w->fd, 0, SEEK_END) < 0 ||
        write_exact(w->fd, crc_buf, 4) < 0)
        goto fail;

    if (fsync(w->fd) < 0)
        goto fail;
    close(w->fd);
    w->fd = -1;

    if (rename(w->tmp_path, w->final_path) < 0) {
        LOG_ERROR("snapshot", "rename failed: %s", strerror(errno));
        unlink(w->tmp_path);
        free(w);
        return -1;
    }

    LOG_INFO("snapshot", "Committed %s: %llu entries, last_index=%llu",
             w->final_path, (unsigned long long)w->num_entries,
             (unsigned long long)w->last_included_index);
    free(w);
    return 0;

fail:
    LOG_ERROR("snapshot", "Commit failed: %s", strerror(errno));
    if (w->fd >= 0)
        close(w->fd);
    unlink(w->tmp_path);
    free(w);
    return -1;
}

void snapshot_abort(snapshot_writer_t *w)
{
    if (!w)
        return;
    if (w->fd >= 0)
        close(w->fd);
    unlink(w->tmp_path);
    free(w);
}

/* -----------------------------------------------------------------------
 * Loader
 * ----------------------------------------------------------------------- */

int64_t snapshot_load(const char *path, snapshot_load_cb cb, void *ctx,
                      uint64_t *out_last_index, uint64_t *out_last_term)
{
    *out_last_index = 0;
    *out_last_term = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT)
            return 0; /* No snapshot yet — not an error */
        LOG_ERROR("snapshot", "open(%s) failed: %s", path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < SNAP_HEADER_SIZE + 4) {
        LOG_ERROR("snapshot", "File too small or stat failed");
        close(fd);
        return -1;
    }
    uint64_t body_len = (uint64_t)st.st_size - 4; /* Excludes footer CRC */

    uint8_t hdr[SNAP_HEADER_SIZE];
    if (read_exact(fd, hdr, sizeof(hdr)) < 0)
        goto corrupt;
    if (get_u32(hdr) != SNAPSHOT_MAGIC || get_u32(hdr + 4) != SNAPSHOT_VERSION)
        goto corrupt;

    uint64_t last_index = get_u64(hdr + 8);
    uint64_t last_term = get_u64(hdr + 16);
    uint64_t num_entries = get_u64(hdr + 24);

    /* Verify CRC over version..end-of-entries in one streaming pass. */
    uint32_t crc = crc32_update(0, hdr + 4, SNAP_HEADER_SIZE - 4);
    {
        uint8_t buf[65536];
        uint64_t off = SNAP_HEADER_SIZE;
        while (off < body_len) {
            size_t chunk = (body_len - off) < sizeof(buf)
                               ? (size_t)(body_len - off) : sizeof(buf);
            ssize_t n = pread(fd, buf, chunk, (off_t)off);
            if (n <= 0)
                goto corrupt;
            crc = crc_chain(crc, buf, (size_t)n);
            off += (uint64_t)n;
        }
    }
    uint8_t crc_buf[4];
    if (pread(fd, crc_buf, 4, (off_t)body_len) != 4)
        goto corrupt;
    if (get_u32(crc_buf) != crc) {
        LOG_ERROR("snapshot", "CRC mismatch — snapshot corrupt");
        close(fd);
        return -1;
    }

    /* Stream the entries. */
    if (lseek(fd, SNAP_HEADER_SIZE, SEEK_SET) < 0)
        goto corrupt;

    char *key = NULL, *value = NULL;
    uint32_t key_cap = 0, value_cap = 0;
    int64_t loaded = 0;

    for (uint64_t i = 0; i < num_entries; i++) {
        uint8_t len_buf[4];
        uint32_t key_len, value_len;

        if (read_exact(fd, len_buf, 4) < 0)
            goto entry_fail;
        key_len = get_u32(len_buf);
        if (key_len > key_cap) {
            void *p = realloc(key, key_len + 1);
            if (!p)
                goto entry_fail;
            key = p;
            key_cap = key_len;
        }
        if (key_len > 0 && read_exact(fd, key, key_len) < 0)
            goto entry_fail;

        if (read_exact(fd, len_buf, 4) < 0)
            goto entry_fail;
        value_len = get_u32(len_buf);
        if (value_len > value_cap) {
            void *p = realloc(value, value_len + 1);
            if (!p)
                goto entry_fail;
            value = p;
            value_cap = value_len;
        }
        if (value_len > 0 && read_exact(fd, value, value_len) < 0)
            goto entry_fail;

        if (cb && cb(key, key_len, value, value_len, ctx) != 0)
            break;
        loaded++;
    }

    free(key);
    free(value);
    close(fd);
    *out_last_index = last_index;
    *out_last_term = last_term;
    LOG_INFO("snapshot", "Loaded %s: %lld entries, last_index=%llu",
             path, (long long)loaded, (unsigned long long)last_index);
    return loaded;

entry_fail:
    free(key);
    free(value);
corrupt:
    LOG_ERROR("snapshot", "Corrupt snapshot file: %s", path);
    close(fd);
    return -1;
}
