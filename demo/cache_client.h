#ifndef CACHE_CLIENT_H
#define CACHE_CLIENT_H

#include "mongoose.h"

struct cache_client {
    struct mg_mgr manager;
    struct mg_connection *connection;
    const char *url;
    char namespace[33];
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

#endif
