#ifndef CACHE_CLIENT_H
#define CACHE_CLIENT_H

#include "mongoose.h"

/* 실행 식별자를 만드는 난수의 바이트 수입니다. 16진수로 쓰면 두 배 길이가 됩니다. */
#define NAMESPACE_BYTES 16

struct cache_client {
    struct mg_mgr manager;
    struct mg_connection *connection;
    const char *url;
    char namespace[NAMESPACE_BYTES * 2 + 1];
    int ttl_seconds;
    int timeout_ms;
    bool enabled;
    bool active;
    bool pending;
    bool done;
    int status;
    char *body;
    uint64_t gets;
    uint64_t hits;
    uint64_t misses;
    uint64_t get_errors;
    uint64_t bypasses;
    uint64_t puts;
    uint64_t put_failures;
    uint64_t deletes;
    uint64_t delete_failures;
    uint64_t probes;
    uint64_t probe_failures;
    uint64_t recoveries;
    uint64_t probe_ms;
};

bool cache_client_init(struct cache_client *cache, const char *url,
                       int ttl_seconds, int timeout_ms);
void cache_client_destroy(struct cache_client *cache);
/* A hit transfers an allocated JSON body to the caller; NULL means origin. */
char *cache_client_get(struct cache_client *cache, const char *key);
void cache_client_set(struct cache_client *cache, const char *key,
                      const char *body);
/* A missing key is a successful invalidation; other failures suspend the cache. */
void cache_client_delete(struct cache_client *cache, const char *key);
/* Probes at most once per second while suspended; true means a new namespace. */
bool cache_client_recover(struct cache_client *cache);

#endif
