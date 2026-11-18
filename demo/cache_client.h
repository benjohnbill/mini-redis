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
    /* 지금 요청에 HTTP 응답이 왔는지입니다. 응답 없이 닫힌 연결을 가립니다. */
    bool answered;
    int status;
    char *body;
    size_t body_len;
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
    /* POST /mget을 보낸 횟수와 그 key 수의 합입니다. */
    uint64_t mgets;
    uint64_t mget_keys;
    /* POST /mset을 보낸 횟수와 그 key 수의 합입니다. 실패는 put_failures에
       더합니다. */
    uint64_t msets;
    uint64_t mset_keys;
};

/* cache_client_mget 한 번의 결과입니다. */
enum cache_mget_result {
    /* 캐시를 쓰지 않는 중이라 보내지 않았습니다. 모든 key가 원본 조회입니다. */
    CACHE_MGET_BYPASSED,
    /* bodies에 key마다 적중한 body나 NULL(미적중)을 두었습니다. */
    CACHE_MGET_DONE,
    /* mini-redis가 400 response_too_large로 거절했습니다. 캐시는 계속 쓰고,
       부른 쪽이 key마다 cache_client_get으로 다시 묻습니다. */
    CACHE_MGET_TOO_LARGE,
    /* 연결 실패, 기한 초과, 깨진 응답입니다. GET 실패처럼 캐시를 중단했습니다. */
    CACHE_MGET_FAILED,
};

bool cache_client_init(struct cache_client *cache, const char *url,
                       int ttl_seconds, int timeout_ms);
void cache_client_destroy(struct cache_client *cache);
/* A hit transfers an allocated JSON body to the caller; NULL means origin. */
char *cache_client_get(struct cache_client *cache, const char *key);
/* keys를 POST /mget 한 번으로 묻습니다. bodies[i]에는 keys[i]가 적중하면
   할당한 body를, 아니면 NULL을 둡니다. 결과와 상관없이 bodies는 모두 채우고,
   NULL이 아닌 body는 부른 쪽이 해제합니다. 통계는 key마다 GET 한 번처럼
   gets, hits, misses, get_errors, bypasses에 더합니다. */
enum cache_mget_result cache_client_mget(struct cache_client *cache,
                                         const char *const *keys,
                                         size_t key_count, char **bodies);
void cache_client_set(struct cache_client *cache, const char *key,
                      const char *body);
/* keys[i]에 bodies[i]를 POST /mset 한 번으로 저장합니다. 캐시를 쓰지 않는
   중이면 보내지 않습니다. 204가 아니면 PUT 실패처럼 put_failures에 더하고 캐시를
   중단합니다. mini-redis가 메모리 부족으로 앞 key만 저장했을 수 있지만, 중단한
   캐시는 재개할 때 새 실행 식별자를 쓰므로 그 key는 다시 읽히지 않습니다. */
void cache_client_mset(struct cache_client *cache, const char *const *keys,
                       const char *const *bodies, size_t key_count);
/* A missing key is a successful invalidation; other failures suspend the cache. */
void cache_client_delete(struct cache_client *cache, const char *key);
/* Probes at most once per second while suspended; true means a new namespace. */
bool cache_client_recover(struct cache_client *cache);

#endif
