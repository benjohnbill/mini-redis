#include "cache_client.h"
#include <stdlib.h>
#include <string.h>

#define CACHE_POLL_MS 5
#define CACHE_KEY_MAX 4096
#define NAMESPACE_BYTES 16
#define RECOVER_INTERVAL_MS 1000

static void cache_event_handle(struct mg_connection *connection, int event,
                               void *event_data);
static int cache_request(struct cache_client *cache, const char *method,
                         const char *key, const char *body);
static bool request_send(struct cache_client *cache, const char *method,
                         const char *key, const char *body);
static bool namespace_new(struct cache_client *cache);
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

static int cache_request(struct cache_client *cache, const char *method,
                         const char *key, const char *body)
{
    uint64_t deadline = mg_millis() + (uint64_t)cache->timeout_ms;
    cache->done = false;
    cache->pending = true;
    cache->status = 0;
    free(cache->body);
    cache->body = NULL;
    if (cache->connection == NULL) {
        cache->connection = mg_http_connect(&cache->manager, cache->url,
                                            cache_event_handle, cache);
    }
    if (cache->connection == NULL || !request_send(cache, method, key, body)) {
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

static void cache_event_handle(struct mg_connection *connection, int event,
                               void *event_data)
{
    struct cache_client *cache = connection->fn_data;
    if (event == MG_EV_HTTP_MSG && cache->pending) {
        struct mg_http_message *message = event_data;
        cache->status = mg_http_status(message);
        if (cache->status == 404) {
            char *reason = mg_json_get_str(message->body, "$.error");
            if (reason == NULL || strcmp(reason, "key_not_found") != 0) {
                cache->status = 0;
            }
            free(reason);
        }
        if (cache->status == 200) {
            cache->body = malloc(message->body.len + 1);
            if (cache->body == NULL) {
                cache->status = 0;
            } else {
                memcpy(cache->body, message->body.buf, message->body.len);
                cache->body[message->body.len] = '\0';
            }
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

static bool request_send(struct cache_client *cache, const char *method,
                         const char *key, const char *body)
{
    char complete[CACHE_KEY_MAX];
    int length = snprintf(complete, sizeof complete, "demo:%s:%s",
                          cache->namespace, key);
    if (length < 0 || (size_t)length >= sizeof complete) {
        return false;
    }
    char encoded[CACHE_KEY_MAX * 3];
    size_t encoded_len =
        mg_url_encode(complete, (size_t)length, encoded, sizeof encoded);
    if (encoded_len == 0 || encoded_len >= sizeof encoded) {
        return false;
    }
    struct mg_str host = mg_url_host(cache->url);
    size_t body_len = body == NULL ? 0 : strlen(body);
    if (mg_printf(cache->connection,
                  "%s /keys/%.*s HTTP/1.1\r\nHost: %.*s:%hu\r\n"
                  "Content-Length: %zu\r\nTTL: %d\r\n\r\n",
                  method, (int)encoded_len, encoded, (int)host.len, host.buf,
                  mg_url_port(cache->url), body_len, cache->ttl_seconds) == 0) {
        return false;
    }

    return body_len == 0 || mg_send(cache->connection, body, body_len);
}

static bool namespace_new(struct cache_client *cache)
{
    unsigned char random[NAMESPACE_BYTES];
    if (!mg_random(random, sizeof random)) {
        return false;
    }
    mg_snprintf(cache->namespace, sizeof cache->namespace, "%M", mg_print_hex,
                (int)sizeof random, random);

    return true;
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
}
