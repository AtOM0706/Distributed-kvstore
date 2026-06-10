#include "kvstore.h"
#include "common/hash.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

#define KV_HASH_SEED 0x9747b28cu

static size_t bucket_for(const kvstore_t *kv, const char *key,
                         uint32_t key_len)
{
    return murmurhash3_32(key, key_len, KV_HASH_SEED) & (kv->capacity - 1);
}

int kvstore_init(kvstore_t *kv)
{
    memset(kv, 0, sizeof(*kv));
    kv->capacity = KV_INITIAL_CAPACITY;
    kv->buckets = calloc(kv->capacity, sizeof(kv_entry_t *));
    if (!kv->buckets)
        return -1;
    pthread_rwlock_init(&kv->lock, NULL);
    return 0;
}

static void free_all_entries(kvstore_t *kv)
{
    for (size_t i = 0; i < kv->capacity; i++) {
        kv_entry_t *e = kv->buckets[i];
        while (e) {
            kv_entry_t *next = e->next;
            free(e->key);
            free(e->value);
            free(e);
            e = next;
        }
        kv->buckets[i] = NULL;
    }
    kv->count = 0;
}

void kvstore_free(kvstore_t *kv)
{
    free_all_entries(kv);
    free(kv->buckets);
    kv->buckets = NULL;
    pthread_rwlock_destroy(&kv->lock);
}

/* Double the bucket array and rehash. Called with the write lock held. */
static int kvstore_resize(kvstore_t *kv)
{
    size_t new_cap = kv->capacity * 2;
    kv_entry_t **new_buckets = calloc(new_cap, sizeof(kv_entry_t *));
    if (!new_buckets)
        return -1;

    for (size_t i = 0; i < kv->capacity; i++) {
        kv_entry_t *e = kv->buckets[i];
        while (e) {
            kv_entry_t *next = e->next;
            size_t b = murmurhash3_32(e->key, e->key_len, KV_HASH_SEED) &
                       (new_cap - 1);
            e->next = new_buckets[b];
            new_buckets[b] = e;
            e = next;
        }
    }
    free(kv->buckets);
    kv->buckets = new_buckets;
    kv->capacity = new_cap;
    LOG_DEBUG("kvstore", "Resized to %zu buckets (%zu keys)", new_cap,
              kv->count);
    return 0;
}

int kvstore_set(kvstore_t *kv, const char *key, uint32_t key_len,
                const char *value, uint32_t value_len)
{
    pthread_rwlock_wrlock(&kv->lock);
    kv->total_sets++;

    size_t b = bucket_for(kv, key, key_len);
    for (kv_entry_t *e = kv->buckets[b]; e; e = e->next) {
        if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0) {
            /* Update in place */
            char *nv = malloc(value_len + 1);
            if (!nv) {
                pthread_rwlock_unlock(&kv->lock);
                return -1;
            }
            memcpy(nv, value, value_len);
            nv[value_len] = '\0';
            free(e->value);
            e->value = nv;
            e->value_len = value_len;
            pthread_rwlock_unlock(&kv->lock);
            return 0;
        }
    }

    /* Insert new entry */
    kv_entry_t *e = malloc(sizeof(*e));
    if (!e) {
        pthread_rwlock_unlock(&kv->lock);
        return -1;
    }
    e->key = malloc(key_len + 1);
    e->value = malloc(value_len + 1);
    if (!e->key || !e->value) {
        free(e->key);
        free(e->value);
        free(e);
        pthread_rwlock_unlock(&kv->lock);
        return -1;
    }
    memcpy(e->key, key, key_len);
    e->key[key_len] = '\0';
    e->key_len = key_len;
    memcpy(e->value, value, value_len);
    e->value[value_len] = '\0';
    e->value_len = value_len;
    e->next = kv->buckets[b];
    kv->buckets[b] = e;
    kv->count++;

    if ((double)kv->count / (double)kv->capacity > KV_MAX_LOAD_FACTOR)
        kvstore_resize(kv);

    pthread_rwlock_unlock(&kv->lock);
    return 0;
}

bool kvstore_get(kvstore_t *kv, const char *key, uint32_t key_len,
                 const char **value, uint32_t *value_len)
{
    pthread_rwlock_rdlock(&kv->lock);
    kv->total_gets++;

    size_t b = bucket_for(kv, key, key_len);
    for (kv_entry_t *e = kv->buckets[b]; e; e = e->next) {
        if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0) {
            if (value)
                *value = e->value;
            if (value_len)
                *value_len = e->value_len;
            pthread_rwlock_unlock(&kv->lock);
            return true;
        }
    }
    pthread_rwlock_unlock(&kv->lock);
    return false;
}

bool kvstore_del(kvstore_t *kv, const char *key, uint32_t key_len)
{
    pthread_rwlock_wrlock(&kv->lock);
    kv->total_dels++;

    size_t b = bucket_for(kv, key, key_len);
    kv_entry_t **pp = &kv->buckets[b];
    while (*pp) {
        kv_entry_t *e = *pp;
        if (e->key_len == key_len && memcmp(e->key, key, key_len) == 0) {
            *pp = e->next;
            free(e->key);
            free(e->value);
            free(e);
            kv->count--;
            pthread_rwlock_unlock(&kv->lock);
            return true;
        }
        pp = &e->next;
    }
    pthread_rwlock_unlock(&kv->lock);
    return false;
}

void kvstore_clear(kvstore_t *kv)
{
    pthread_rwlock_wrlock(&kv->lock);
    free_all_entries(kv);
    pthread_rwlock_unlock(&kv->lock);
}

size_t kvstore_count(kvstore_t *kv)
{
    pthread_rwlock_rdlock(&kv->lock);
    size_t n = kv->count;
    pthread_rwlock_unlock(&kv->lock);
    return n;
}

void kvstore_foreach(kvstore_t *kv, kvstore_iter_cb cb, void *ctx)
{
    pthread_rwlock_rdlock(&kv->lock);
    for (size_t i = 0; i < kv->capacity; i++) {
        for (kv_entry_t *e = kv->buckets[i]; e; e = e->next) {
            if (cb(e->key, e->key_len, e->value, e->value_len, ctx) != 0)
                goto done;
        }
    }
done:
    pthread_rwlock_unlock(&kv->lock);
}
