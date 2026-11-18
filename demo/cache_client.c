#include "cache_client.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_POLL_MS 5
#define CACHE_KEY_MAX 4096
#define RECOVER_INTERVAL_MS 1000
/* URL 인코딩은 바이트 하나를 %XX 세 글자로 늘립니다. */
#define URL_ENCODED_BYTES_MAX 3
#define ENCODED_KEY_MAX (CACHE_KEY_MAX * URL_ENCODED_BYTES_MAX)
#define KEYS_PATH "/keys/"
#define MGET_PATH "/mget"
#define MSET_PATH "/mset"
/* "<value 바이트 길이>\n"의 최대 길이입니다. size_t 20자리와 \n, '\0'이
   들어갑니다. */
#define MSET_LENGTH_TEXT_MAX 24
/* POST /mget 응답에서 없는 key 하나의 자리입니다(docs/api.md). */
#define MGET_MISSING "-\n"
#define MGET_MISSING_LEN 2
#define DECIMAL_BASE 10

static bool namespace_new(struct cache_client *cache);
static int cache_request(struct cache_client *cache, const char *method,
                         const char *key, const char *body);
static size_t key_encode(const struct cache_client *cache, const char *key,
                         char *encoded, size_t encoded_size);
static int cache_exchange(struct cache_client *cache, const char *method,
                          const char *path, size_t path_len, const char *body,
                          size_t body_len);
static int request_exchange(struct cache_client *cache, const char *method,
                            const char *path, size_t path_len, const char *body,
                            size_t body_len);
static void request_state_reset(struct cache_client *cache);
static void cache_event_handle(struct mg_connection *connection, int event,
                               void *event_data);
static bool reason_is(const struct mg_http_message *message,
                      const char *reason);
static void response_body_copy(struct cache_client *cache,
                               const struct mg_http_message *message);
static bool request_send(struct cache_client *cache, const char *method,
                         const char *path, size_t path_len, const char *body,
                         size_t body_len);
static char *mset_body_create(const struct cache_client *cache,
                              const char *const *keys,
                              const char *const *bodies, size_t key_count,
                              size_t *body_len);
static char *mget_body_create(const struct cache_client *cache,
                              const char *const *keys, size_t key_count,
                              size_t *body_len);
static bool mget_response_parse(const char *text, size_t text_len,
                                size_t key_count, char **bodies);
static size_t mget_value_parse(const char *text, size_t text_len, char **body);
static void bodies_free(char **bodies, size_t key_count);
static void cache_suspend(struct cache_client *cache);
static void cache_connection_close(struct cache_client *cache);

bool cache_client_init(struct cache_client *cache, const char *url,
                       int ttl_seconds, int timeout_ms)
{
    mg_mgr_init(&cache->manager);
    cache->url = url;
    cache->enabled = url != NULL;
    cache->active = cache->enabled;
    cache->ttl_seconds = ttl_seconds;
    cache->timeout_ms = timeout_ms;
    if (cache->enabled && !namespace_new(cache)) {
        return false;
    }

    return true;
}

void cache_client_destroy(struct cache_client *cache)
{
    mg_mgr_free(&cache->manager);
    free(cache->body);
}

char *cache_client_get(struct cache_client *cache, const char *key)
{
    if (!cache->active) {
        cache->bypasses++;
        return NULL;
    }
    cache->gets++;
    int status = cache_request(cache, "GET", key, NULL);
    if (status == 200) {
        cache->hits++;
        char *body = cache->body;
        cache->body = NULL;
        return body;
    }
    if (status == 404) {
        cache->misses++;
    } else {
        cache->get_errors++;
        cache_suspend(cache);
    }

    return NULL;
}

enum cache_mget_result cache_client_mget(struct cache_client *cache,
                                         const char *const *keys,
                                         size_t key_count, char **bodies)
{
    for (size_t i = 0; i < key_count; i++) {
        bodies[i] = NULL;
    }
    if (!cache->active) {
        cache->bypasses += key_count;
        return CACHE_MGET_BYPASSED;
    }
    size_t request_len = 0;
    char *request = mget_body_create(cache, keys, key_count, &request_len);
    int status = 0;
    if (request != NULL) {
        cache->mgets++;
        cache->mget_keys += key_count;
        status = cache_exchange(cache, "POST", MGET_PATH, strlen(MGET_PATH),
                                request, request_len);
        free(request);
    }
    /* 거절된 묶음은 부른 쪽이 key마다 GET으로 다시 물으므로 gets에 더하지
       않습니다. */
    if (status == 400) {
        return CACHE_MGET_TOO_LARGE;
    }
    cache->gets += key_count;
    if (status == 200 &&
        mget_response_parse(cache->body, cache->body_len, key_count, bodies)) {
        for (size_t i = 0; i < key_count; i++) {
            if (bodies[i] != NULL) {
                cache->hits++;
            } else {
                cache->misses++;
            }
        }
        return CACHE_MGET_DONE;
    }
    /* 깨진 응답도 GET 실패와 같게 봅니다. */
    bodies_free(bodies, key_count);
    cache->get_errors += key_count;
    cache_suspend(cache);

    return CACHE_MGET_FAILED;
}

void cache_client_set(struct cache_client *cache, const char *key,
                      const char *body)
{
    if (!cache->active) {
        return;
    }
    cache->puts++;
    if (cache_request(cache, "PUT", key, body) != 204) {
        cache->put_failures++;
        cache_suspend(cache);
    }
}

void cache_client_mset(struct cache_client *cache, const char *const *keys,
                       const char *const *bodies, size_t key_count)
{
    if (!cache->active) {
        return;
    }
    cache->msets++;
    cache->mset_keys += key_count;
    size_t request_len = 0;
    char *request =
        mset_body_create(cache, keys, bodies, key_count, &request_len);
    int status = 0;
    if (request != NULL) {
        status = cache_exchange(cache, "POST", MSET_PATH, strlen(MSET_PATH),
                                request, request_len);
        free(request);
    }
    if (status != 204) {
        cache->put_failures++;
        cache_suspend(cache);
    }
}

void cache_client_delete(struct cache_client *cache, const char *key)
{
    if (!cache->active) {
        return;
    }
    cache->deletes++;
    int status = cache_request(cache, "DELETE", key, NULL);
    if (status != 204 && status != 404) {
        cache->delete_failures++;
        cache_suspend(cache);
    }
}

bool cache_client_recover(struct cache_client *cache)
{
    if (!cache->enabled || cache->active) {
        return false;
    }
    uint64_t now = mg_millis();
    if (cache->probe_ms != 0 && now - cache->probe_ms < RECOVER_INTERVAL_MS) {
        return false;
    }
    cache->probe_ms = now;
    cache->probes++;
    if (cache_request(cache, "GET", "probe", NULL) != 404) {
        cache->probe_failures++;
        cache_connection_close(cache);
        return false;
    }
    if (!namespace_new(cache)) {
        cache_connection_close(cache);
        return false;
    }
    cache->recoveries++;
    cache->active = true;

    return true;
}

static bool namespace_new(struct cache_client *cache)
{
    unsigned char random_bytes[NAMESPACE_BYTES];
    if (!mg_random(random_bytes, sizeof random_bytes)) {
        return false;
    }
    mg_snprintf(cache->namespace, sizeof cache->namespace, "%M", mg_print_hex,
                (int)sizeof random_bytes, random_bytes);

    return true;
}

/* /keys/<인코딩한 key>에 요청을 보냅니다. key를 인코딩할 수 없으면 보내지
   못한 요청처럼 상태 0입니다. */
static int cache_request(struct cache_client *cache, const char *method,
                         const char *key, const char *body)
{
    char path[sizeof KEYS_PATH + ENCODED_KEY_MAX];
    size_t prefix_len = strlen(KEYS_PATH);
    memcpy(path, KEYS_PATH, prefix_len);
    size_t encoded_len =
        key_encode(cache, key, path + prefix_len, sizeof path - prefix_len);
    size_t path_len = encoded_len == 0 ? 0 : prefix_len + encoded_len;

    return cache_exchange(cache, method, path, path_len, body,
                          body == NULL ? 0 : strlen(body));
}

/* "demo:<실행 식별자>:<key>"를 퍼센트 인코딩해 encoded에 쓰고 그 길이를
   돌려줍니다. 담을 수 없으면 0입니다. */
static size_t key_encode(const struct cache_client *cache, const char *key,
                         char *encoded, size_t encoded_size)
{
    char full_key[CACHE_KEY_MAX];
    int length = snprintf(full_key, sizeof full_key, "demo:%s:%s",
                          cache->namespace, key);
    if (length < 0 || (size_t)length >= sizeof full_key) {
        return 0;
    }
    size_t encoded_len =
        mg_url_encode(full_key, (size_t)length, encoded, encoded_size);

    return encoded_len >= encoded_size ? 0 : encoded_len;
}

/* 요청 하나를 보내고 응답이나 기한을 기다려 상태 코드를 돌려줍니다. path_len이
   0이면 보내지 못한 요청처럼 상태 0입니다.
   mini-redis는 쉬는 연결을 닫지만(concurrency spec Q4), 이 client는 요청이
   없는 동안 manager를 poll하지 않으므로 그 닫힘을 다음 요청을 보낸 뒤에야
   압니다. 다시 쓴 연결이 응답을 하나도 주지 않고 닫혔으면 캐시 오류로 보지
   않고 새 연결로 한 번 더 보냅니다. 쉬는 연결은 받은 요청이 없을 때만 닫히므로
   서버는 첫 요청을 처리하지 않았고, 캐시 요청은 모두 여러 번 보내도 결과가
   같습니다. */
static int cache_exchange(struct cache_client *cache, const char *method,
                          const char *path, size_t path_len, const char *body,
                          size_t body_len)
{
    bool is_reused = cache->connection != NULL;
    int status =
        request_exchange(cache, method, path, path_len, body, body_len);
    if (is_reused && cache->connection == NULL && !cache->answered) {
        status =
            request_exchange(cache, method, path, path_len, body, body_len);
    }

    return status;
}

/* 연결이 없으면 새로 열고, 요청 하나를 보내 응답이나 기한을 기다립니다. */
static int request_exchange(struct cache_client *cache, const char *method,
                            const char *path, size_t path_len, const char *body,
                            size_t body_len)
{
    uint64_t deadline = mg_millis() + (uint64_t)cache->timeout_ms;
    request_state_reset(cache);
    if (cache->connection == NULL) {
        cache->connection = mg_http_connect(&cache->manager, cache->url,
                                            cache_event_handle, cache);
    }
    if (cache->connection == NULL || path_len == 0 ||
        !request_send(cache, method, path, path_len, body, body_len)) {
        cache->done = true;
    }
    /* Poll only the client manager: product handlers must never reenter. */
    uint64_t now = mg_millis();
    while (!cache->done && now < deadline) {
        uint64_t remaining = deadline - now;
        mg_mgr_poll(&cache->manager,
                    remaining < CACHE_POLL_MS ? (int)remaining : CACHE_POLL_MS);
        now = mg_millis();
    }
    if (!cache->done) {
        cache->status = 0;
    }
    cache->pending = false;

    return cache->status;
}

static void request_state_reset(struct cache_client *cache)
{
    cache->done = false;
    cache->pending = true;
    cache->answered = false;
    cache->status = 0;
    free(cache->body);
    cache->body = NULL;
    cache->body_len = 0;
}

/* 404는 key_not_found일 때만, 400은 POST /mget의 response_too_large일 때만
   그 상태로 남깁니다. 다른 이유의 404와 400은 캐시 오류(상태 0)입니다. */
static void cache_event_handle(struct mg_connection *connection, int event,
                               void *event_data)
{
    struct cache_client *cache = connection->fn_data;
    if (event == MG_EV_HTTP_MSG && cache->pending) {
        struct mg_http_message *message = event_data;
        cache->answered = true;
        cache->status = mg_http_status(message);
        if ((cache->status == 404 && !reason_is(message, "key_not_found")) ||
            (cache->status == 400 &&
             !reason_is(message, "response_too_large"))) {
            cache->status = 0;
        }
        if (cache->status == 200) {
            response_body_copy(cache, message);
        }
        cache->done = true;
    } else if (event == MG_EV_ERROR) {
        cache->status = 0;
        cache->done = true;
    } else if (event == MG_EV_CLOSE) {
        cache->connection = NULL;
        if (cache->pending && !cache->done) {
            cache->status = 0;
            cache->done = true;
        }
    }
}

static bool reason_is(const struct mg_http_message *message, const char *reason)
{
    char *actual = mg_json_get_str(message->body, "$.error");
    bool is_same = actual != NULL && strcmp(actual, reason) == 0;
    free(actual);

    return is_same;
}

/* Copies the body as a NUL-terminated string; no memory turns the hit into an
   error. */
static void response_body_copy(struct cache_client *cache,
                               const struct mg_http_message *message)
{
    cache->body = malloc(message->body.len + 1);
    if (cache->body == NULL) {
        cache->status = 0;
        return;
    }
    memcpy(cache->body, message->body.buf, message->body.len);
    cache->body[message->body.len] = '\0';
    cache->body_len = message->body.len;
}

static bool request_send(struct cache_client *cache, const char *method,
                         const char *path, size_t path_len, const char *body,
                         size_t body_len)
{
    struct mg_str host = mg_url_host(cache->url);
    if (mg_printf(cache->connection,
                  "%s %.*s HTTP/1.1\r\nHost: %.*s:%hu\r\n"
                  "Content-Length: %zu\r\nTTL: %d\r\n\r\n",
                  method, (int)path_len, path, (int)host.len, host.buf,
                  mg_url_port(cache->url), body_len, cache->ttl_seconds) == 0) {
        return false;
    }

    return body_len == 0 || mg_send(cache->connection, body, body_len);
}

/* key마다 "<인코딩한 key>\n<body 바이트 길이>\n<body>\n"을 이은 POST /mset
   body를 만듭니다(docs/api.md "여러 key 저장"). 인코딩한 key에는 \n이 없습니다.
   담을 수 없는 key가 있거나 메모리가 없으면 NULL입니다. */
static char *mset_body_create(const struct cache_client *cache,
                              const char *const *keys,
                              const char *const *bodies, size_t key_count,
                              size_t *body_len)
{
    char encoded[ENCODED_KEY_MAX];
    char length_text[MSET_LENGTH_TEXT_MAX];
    char *body = NULL;
    size_t written = 0;
    for (size_t i = 0; i < key_count; i++) {
        size_t encoded_len =
            key_encode(cache, keys[i], encoded, sizeof encoded);
        size_t value_len = strlen(bodies[i]);
        int length_text_len =
            snprintf(length_text, sizeof length_text, "%zu\n", value_len);
        size_t item_len =
            encoded_len + 1 + (size_t)length_text_len + value_len + 1;
        char *grown =
            encoded_len == 0 ? NULL : realloc(body, written + item_len);
        if (grown == NULL) {
            free(body);
            return NULL;
        }
        body = grown;
        memcpy(body + written, encoded, encoded_len);
        written += encoded_len;
        body[written++] = '\n';
        memcpy(body + written, length_text, (size_t)length_text_len);
        written += (size_t)length_text_len;
        memcpy(body + written, bodies[i], value_len);
        written += value_len;
        body[written++] = '\n';
    }
    *body_len = written;

    return body;
}

/* key마다 /keys/<key>와 같은 인코딩을 한 줄씩 이은 POST /mget body를
   만듭니다. 인코딩한 key에는 \n이 없습니다. 담을 수 없는 key가 있거나 메모리가
   없으면 NULL입니다. */
static char *mget_body_create(const struct cache_client *cache,
                              const char *const *keys, size_t key_count,
                              size_t *body_len)
{
    char encoded[ENCODED_KEY_MAX];
    char *body = NULL;
    size_t written = 0;
    for (size_t i = 0; i < key_count; i++) {
        size_t encoded_len =
            key_encode(cache, keys[i], encoded, sizeof encoded);
        char *grown =
            encoded_len == 0 ? NULL : realloc(body, written + encoded_len + 1);
        if (grown == NULL) {
            free(body);
            return NULL;
        }
        body = grown;
        memcpy(body + written, encoded, encoded_len);
        written += encoded_len;
        body[written++] = '\n';
    }
    *body_len = written;

    return body;
}

/* POST /mget 응답을 key 순서대로 "<길이>\n<바이트>\n" 또는 "-\n"으로 읽어
   bodies에 둡니다. key 수와 맞지 않거나 형식이 깨졌으면 false를 돌려주며, 이미
   만든 body는 부른 쪽이 bodies_free로 해제합니다. */
static bool mget_response_parse(const char *text, size_t text_len,
                                size_t key_count, char **bodies)
{
    size_t offset = 0;
    for (size_t i = 0; i < key_count; i++) {
        if (text_len - offset >= MGET_MISSING_LEN &&
            memcmp(text + offset, MGET_MISSING, MGET_MISSING_LEN) == 0) {
            offset += MGET_MISSING_LEN;
            continue;
        }
        size_t used =
            mget_value_parse(text + offset, text_len - offset, &bodies[i]);
        if (used == 0) {
            return false;
        }
        offset += used;
    }

    return offset == text_len;
}

/* "<10진수 길이>\n<바이트>\n" 하나를 읽어 바이트를 '\0'으로 끝나는 새 body로
   *body에 두고, 읽은 바이트 수를 돌려줍니다. 형식이 깨졌거나 메모리가 없으면
   0입니다. */
static size_t mget_value_parse(const char *text, size_t text_len, char **body)
{
    size_t offset = 0;
    size_t value_len = 0;
    while (offset < text_len && text[offset] >= '0' && text[offset] <= '9') {
        size_t digit = (size_t)(text[offset] - '0');
        if (value_len > (SIZE_MAX - digit) / DECIMAL_BASE) {
            return 0;
        }
        value_len = value_len * DECIMAL_BASE + digit;
        offset++;
    }
    if (offset == 0 || offset >= text_len || text[offset] != '\n' ||
        value_len >= text_len - offset - 1 ||
        text[offset + 1 + value_len] != '\n') {
        return 0;
    }
    *body = malloc(value_len + 1);
    if (*body == NULL) {
        return 0;
    }
    memcpy(*body, text + offset + 1, value_len);
    (*body)[value_len] = '\0';

    return offset + 1 + value_len + 1;
}

static void bodies_free(char **bodies, size_t key_count)
{
    for (size_t i = 0; i < key_count; i++) {
        free(bodies[i]);
        bodies[i] = NULL;
    }
}

static void cache_suspend(struct cache_client *cache)
{
    cache->active = false;
    cache_connection_close(cache);
}

static void cache_connection_close(struct cache_client *cache)
{
    if (cache->connection != NULL) {
        cache->connection->is_closing = 1;
        /* The request callback has returned; release its socket and buffers. */
        mg_mgr_poll(&cache->manager, 0);
        cache->connection = NULL;
    }
    free(cache->body);
    cache->body = NULL;
    cache->body_len = 0;
}
