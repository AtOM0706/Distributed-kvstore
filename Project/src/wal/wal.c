#include "wal.h"
#include "common/log.h"
#include "common/hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v);
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

/* Read exactly `len` bytes at `offset`. Returns 0 on success, -1 on
 * short read or error. */
static int read_exact(int fd, uint64_t offset, void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, (uint8_t *)buf + done, len - done,
                          (off_t)(offset + done));
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
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

static int offsets_push(wal_t *wal, uint64_t index, uint64_t offset)
{
    if (wal->offsets_count == wal->offsets_cap) {
        size_t new_cap = wal->offsets_cap ? wal->offsets_cap * 2 : 1024;
        void *p = realloc(wal->offsets, new_cap * sizeof(wal->offsets[0]));
        if (!p)
            return -1;
        wal->offsets = p;
        wal->offsets_cap = new_cap;
    }
    wal->offsets[wal->offsets_count].index = index;
    wal->offsets[wal->offsets_count].offset = offset;
    wal->offsets_count++;
    return 0;
}

/* -----------------------------------------------------------------------
 * Scan the file from the beginning, validating every entry. Rebuilds the
 * offset map. If trailing corruption is found (torn write from a crash),
 * the file is truncated at the last valid entry.
 * ----------------------------------------------------------------------- */
static int wal_scan(wal_t *wal)
{
    struct stat st;
    if (fstat(wal->fd, &st) < 0)
        return -1;

    uint64_t file_size = (uint64_t)st.st_size;
    uint64_t off = 0;
    uint8_t header[WAL_HEADER_SIZE];
    uint8_t *payload = NULL;
    uint32_t payload_cap = 0;

    wal->offsets_count = 0;
    wal->entry_count = 0;
    wal->last_index = 0;

    while (off + WAL_HEADER_SIZE + 4 <= file_size) {
        if (read_exact(wal->fd, off, header, WAL_HEADER_SIZE) < 0)
            break;

        uint32_t magic = get_u32(header);
        uint32_t len = get_u32(header + 4);
        if (magic != WAL_MAGIC || len > WAL_MAX_PAYLOAD)
            break;
        if (off + WAL_HEADER_SIZE + len + 4 > file_size)
            break; /* Torn write: entry extends past EOF */

        if (len > payload_cap) {
            void *p = realloc(payload, len);
            if (!p) {
                free(payload);
                return -1;
            }
            payload = p;
            payload_cap = len;
        }
        if (len > 0 &&
            read_exact(wal->fd, off + WAL_HEADER_SIZE, payload, len) < 0)
            break;

        uint8_t crc_buf[4];
        if (read_exact(wal->fd, off + WAL_HEADER_SIZE + len, crc_buf, 4) < 0)
            break;
        uint32_t stored_crc = get_u32(crc_buf);

        /* CRC covers term..payload (skip magic+len so the checksum also
         * implicitly validates them via the magic check above). */
        uint32_t crc = crc32(header + 8, WAL_HEADER_SIZE - 8);
        if (len > 0) {
            /* Combine: compute over a contiguous concat. Simpler: compute
             * incrementally is not exposed, so checksum header+payload via
             * a small stack trick — compute over payload separately and
             * mix. To keep it simple and robust, CRC the concatenation. */
            uint8_t *tmp = malloc(WAL_HEADER_SIZE - 8 + len);
            if (!tmp) {
                free(payload);
                return -1;
            }
            memcpy(tmp, header + 8, WAL_HEADER_SIZE - 8);
            memcpy(tmp + WAL_HEADER_SIZE - 8, payload, len);
            crc = crc32(tmp, WAL_HEADER_SIZE - 8 + len);
            free(tmp);
        }
        if (crc != stored_crc) {
            LOG_WARN("wal", "CRC mismatch at offset %llu — truncating",
                     (unsigned long long)off);
            break;
        }

        uint64_t index = get_u64(header + 16);
        if (offsets_push(wal, index, off) < 0) {
            free(payload);
            return -1;
        }
        wal->last_index = index;
        wal->entry_count++;
        off += WAL_HEADER_SIZE + len + 4;
    }

    free(payload);

    if (off < file_size) {
        LOG_WARN("wal", "Trimming %llu corrupt/partial trailing bytes",
                 (unsigned long long)(file_size - off));
        if (ftruncate(wal->fd, (off_t)off) < 0)
            return -1;
    }
    wal->file_size = off;
    return 0;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

int wal_open(wal_t *wal, const char *path)
{
    memset(wal, 0, sizeof(*wal));
    snprintf(wal->path, sizeof(wal->path), "%s", path);

    wal->fd = open(path, O_RDWR | O_CREAT, 0644);
    if (wal->fd < 0) {
        LOG_ERROR("wal", "open(%s) failed: %s", path, strerror(errno));
        return -1;
    }

    if (wal_scan(wal) < 0) {
        close(wal->fd);
        wal->fd = -1;
        return -1;
    }

    LOG_INFO("wal", "Opened %s: %llu entries, last_index=%llu, %llu bytes",
             path, (unsigned long long)wal->entry_count,
             (unsigned long long)wal->last_index,
             (unsigned long long)wal->file_size);
    return 0;
}

void wal_close(wal_t *wal)
{
    if (wal->fd >= 0) {
        fsync(wal->fd);
        close(wal->fd);
        wal->fd = -1;
    }
    free(wal->offsets);
    wal->offsets = NULL;
    wal->offsets_count = wal->offsets_cap = 0;
}

int wal_append(wal_t *wal, uint64_t term, uint64_t index, uint8_t cmd_type,
               const void *payload, uint32_t payload_len)
{
    if (payload_len > WAL_MAX_PAYLOAD) {
        LOG_ERROR("wal", "Payload too large: %u bytes", payload_len);
        return -1;
    }

    /* Build the full record in one buffer so a single write() keeps the
     * entry as contiguous as possible (still not atomic — CRC handles
     * torn writes). */
    uint32_t record_len = WAL_HEADER_SIZE + payload_len + 4;
    uint8_t *rec = malloc(record_len);
    if (!rec)
        return -1;

    put_u32(rec, WAL_MAGIC);
    put_u32(rec + 4, payload_len);
    put_u64(rec + 8, term);
    put_u64(rec + 16, index);
    rec[24] = cmd_type;
    if (payload_len > 0)
        memcpy(rec + WAL_HEADER_SIZE, payload, payload_len);

    uint32_t crc = crc32(rec + 8, WAL_HEADER_SIZE - 8 + payload_len);
    put_u32(rec + WAL_HEADER_SIZE + payload_len, crc);

    if (lseek(wal->fd, (off_t)wal->file_size, SEEK_SET) < 0 ||
        write_exact(wal->fd, rec, record_len) < 0) {
        LOG_ERROR("wal", "write failed: %s", strerror(errno));
        free(rec);
        return -1;
    }
    free(rec);
    wal->dirty = true; /* Durable after the next wal_sync() */

    if (offsets_push(wal, index, wal->file_size) < 0)
        return -1;
    wal->file_size += record_len;
    wal->last_index = index;
    wal->entry_count++;
    return 0;
}

int wal_sync(wal_t *wal)
{
    if (!wal->dirty)
        return 0;
    if (fsync(wal->fd) < 0) {
        LOG_ERROR("wal", "fsync failed: %s", strerror(errno));
        return -1;
    }
    wal->dirty = false;
    return 0;
}

int64_t wal_replay(wal_t *wal, wal_replay_cb cb, void *ctx)
{
    uint8_t header[WAL_HEADER_SIZE];
    uint8_t *payload = NULL;
    uint32_t payload_cap = 0;
    int64_t count = 0;

    for (size_t i = 0; i < wal->offsets_count; i++) {
        uint64_t off = wal->offsets[i].offset;
        if (read_exact(wal->fd, off, header, WAL_HEADER_SIZE) < 0) {
            free(payload);
            return -1;
        }
        uint32_t len = get_u32(header + 4);
        if (len > payload_cap) {
            void *p = realloc(payload, len);
            if (!p) {
                free(payload);
                return -1;
            }
            payload = p;
            payload_cap = len;
        }
        if (len > 0 &&
            read_exact(wal->fd, off + WAL_HEADER_SIZE, payload, len) < 0) {
            free(payload);
            return -1;
        }

        uint64_t term = get_u64(header + 8);
        uint64_t index = get_u64(header + 16);
        uint8_t cmd = header[24];

        if (cb(term, index, cmd, payload, len, ctx) != 0)
            break;
        count++;
    }

    free(payload);
    return count;
}

int wal_truncate_after(wal_t *wal, uint64_t index)
{
    /* Find the first entry with index > `index` and cut the file there. */
    size_t keep = wal->offsets_count;
    for (size_t i = 0; i < wal->offsets_count; i++) {
        if (wal->offsets[i].index > index) {
            keep = i;
            break;
        }
    }
    if (keep == wal->offsets_count)
        return 0; /* Nothing to truncate */

    uint64_t new_size = wal->offsets[keep].offset;
    if (ftruncate(wal->fd, (off_t)new_size) < 0) {
        LOG_ERROR("wal", "ftruncate failed: %s", strerror(errno));
        return -1;
    }
    if (fsync(wal->fd) < 0)
        return -1;

    wal->offsets_count = keep;
    wal->entry_count = keep;
    wal->file_size = new_size;
    wal->last_index = keep > 0 ? wal->offsets[keep - 1].index : 0;

    LOG_INFO("wal", "Truncated after index %llu (%zu entries remain)",
             (unsigned long long)index, keep);
    return 0;
}

int wal_truncate_before(wal_t *wal, uint64_t index)
{
    /* Rewrite surviving entries to a temp file, then atomically rename. */
    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s.compact", wal->path);

    int tmp_fd = open(tmp_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (tmp_fd < 0) {
        LOG_ERROR("wal", "open(%s) failed: %s", tmp_path, strerror(errno));
        return -1;
    }

    /* Copy every entry with index > `index` verbatim. */
    uint8_t header[WAL_HEADER_SIZE];
    uint8_t *rec = NULL;
    uint32_t rec_cap = 0;

    for (size_t i = 0; i < wal->offsets_count; i++) {
        if (wal->offsets[i].index <= index)
            continue;
        uint64_t off = wal->offsets[i].offset;
        if (read_exact(wal->fd, off, header, WAL_HEADER_SIZE) < 0)
            goto fail;
        uint32_t len = get_u32(header + 4);
        uint32_t total = WAL_HEADER_SIZE + len + 4;
        if (total > rec_cap) {
            void *p = realloc(rec, total);
            if (!p)
                goto fail;
            rec = p;
            rec_cap = total;
        }
        if (read_exact(wal->fd, off, rec, total) < 0)
            goto fail;
        if (write_exact(tmp_fd, rec, total) < 0)
            goto fail;
    }
    free(rec);
    rec = NULL;

    if (fsync(tmp_fd) < 0)
        goto fail;
    close(tmp_fd);
    tmp_fd = -1;

    if (rename(tmp_path, wal->path) < 0) {
        LOG_ERROR("wal", "rename failed: %s", strerror(errno));
        return -1;
    }

    /* Swap in the new file and rescan. */
    close(wal->fd);
    wal->fd = open(wal->path, O_RDWR, 0644);
    if (wal->fd < 0)
        return -1;
    if (wal_scan(wal) < 0)
        return -1;

    LOG_INFO("wal", "Compacted: removed entries <= %llu, %llu remain",
             (unsigned long long)index,
             (unsigned long long)wal->entry_count);
    return 0;

fail:
    LOG_ERROR("wal", "Compaction failed: %s", strerror(errno));
    free(rec);
    if (tmp_fd >= 0)
        close(tmp_fd);
    unlink(tmp_path);
    return -1;
}
