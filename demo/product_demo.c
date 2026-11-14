#include "mongoose.h"
#include "cache_client.h"
#include "origin.h"
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT_MAX 65535
#define PRODUCT_ID_MAX 2147483647
#define PRODUCT_COUNT_DEFAULT 10000
#define SEED_MAX 2147483646
#define POLL_INTERVAL_MS 10
#define LISTEN_URL_MAX 64
#define CACHE_TTL_DEFAULT 60
#define CACHE_TIMEOUT_DEFAULT 100
#define LIST_LIMIT_DEFAULT 20
#define LIST_LIMIT_MAX 100
#define LIST_OFFSET_MAX 1000000
#define LIST_KEY_MAX 512
#define UPDATE_BODY_MAX 4096
#define LIST_VALUE_RAW_MAX (NAME_MAX_BYTES * 3)
#define LIST_VALUE_SIZE (LIST_VALUE_RAW_MAX + 2)

struct text {
    char *data;
    size_t len;
    size_t capacity;
};

struct items_context {
    struct text *body;
    bool first;
};

struct options {
    const char *database;
    int port;
    int seed;
    int count;
    const char *cache_mode;
    const char *cache_url;
    int ttl_seconds;
    int cache_timeout_ms;
};

struct demo {
    struct origin *origin;
    struct cache_client cache;
    unsigned long long list_generation;
    uint64_t product_gets;
    uint64_t origin_queries;
    uint64_t origin_failures;
    uint64_t origin_changes;
    uint64_t invalidations;
};

static volatile sig_atomic_t stop_requested;
static bool options_parse(int argc, char **argv, struct options *options);
static bool positive_integer_parse(const char *text, int maximum, int *value);
static bool cache_url_is_valid(const char *url);
static bool integer_parse(const char *text, size_t text_len, int maximum,
                          int *value);
static bool items_append(void *context, int product_id, const char *name,
                         size_t name_len, int price);
static int server_run(struct demo *demo, int port);
static void stop_signal_handle(int signum);
static void http_event_handle(struct mg_connection *connection, int event,
                              void *event_data);
static void request_handle(struct mg_connection *connection,
                           struct mg_http_message *message);
static bool method_is(const struct mg_http_message *message,
                      const char *method);
static void method_not_allowed(struct mg_connection *connection,
                               const char *allow);
static void error_send(struct mg_connection *connection, int status,
                       const char *reason);
static void stats_send(struct mg_connection *connection,
                       const struct demo *demo);
static void list_send(struct mg_connection *connection, struct demo *demo,
                      struct mg_str query);
static bool list_query_parse(struct mg_str query, struct list_query *list);
static bool list_pair_apply(const char *pair, size_t pair_len,
                            struct list_query *list, bool *seen);
static bool list_value_decode(const char *text, size_t text_len, char *value,
                              size_t value_size, size_t *value_len);
static void list_key(const struct demo *demo, const struct list_query *list,
                     char *key, size_t key_size);
static char *list_read(struct demo *demo, const struct list_query *list,
                       int *status);
static bool text_append(struct text *text, const char *piece);
static void product_send(struct mg_connection *connection, struct demo *demo,
                         int product_id);
static void product_update(struct mg_connection *connection, struct demo *demo,
                           int product_id, struct mg_http_message *message);
static void product_delete(struct mg_connection *connection, struct demo *demo,
                           int product_id);
static bool content_type_is_json(struct mg_http_message *message);
static bool update_parse(struct mg_str body, char *name, size_t *name_len,
                         int *price);
static void cache_invalidate(struct demo *demo, int product_id);
static void cache_recover(struct demo *demo);
static void body_send(struct mg_connection *connection, const char *body);
static char *product_read(struct demo *demo, int product_id, int *status);
static bool name_is_valid(const unsigned char *name, int name_len);

int main(int argc, char **argv)
{
    struct options options = {.count = PRODUCT_COUNT_DEFAULT};
    if (!options_parse(argc, argv, &options)) {
        fprintf(
            stderr,
            "usage: %s --db PATH --port N --cache on|off [--cache-url URL]\n"
            "       %s --db PATH --seed N [--count N]\n",
            argv[0], argv[0]);
        return 2;
    }
    struct demo demo = {0};
    demo.origin = origin_open(options.database, options.seed != 0);
    if (demo.origin == NULL) {
        return 1;
    }
    int exit_code = 0;
    if (options.seed != 0) {
        exit_code =
            origin_seed(demo.origin, options.seed, options.count) ? 0 : 1;
    } else {
        bool initialized = cache_client_init(
            &demo.cache,
            strcmp(options.cache_mode, "on") == 0 ? options.cache_url : NULL,
            options.ttl_seconds, options.cache_timeout_ms);
        exit_code = initialized ? server_run(&demo, options.port) : 1;
        cache_client_destroy(&demo.cache);
    }
    origin_destroy(demo.origin);

    return exit_code;
}

static bool options_parse(int argc, char **argv, struct options *options)
{

    bool has_count = false;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc) {
            return false;
        }
        const char *name = argv[i];
        const char *value = argv[i + 1];
        if (strcmp(name, "--db") == 0 && options->database == NULL &&
            value[0] != '\0') {
            options->database = value;
        } else if (strcmp(name, "--port") == 0 && options->port == 0) {
            if (!positive_integer_parse(value, PORT_MAX, &options->port)) {
                return false;
            }
        } else if (strcmp(name, "--seed") == 0 && options->seed == 0) {
            if (!positive_integer_parse(value, SEED_MAX, &options->seed)) {
                return false;
            }
        } else if (strcmp(name, "--count") == 0 && !has_count) {
            has_count = true;
            if (!positive_integer_parse(value, PRODUCT_ID_MAX,
                                        &options->count)) {
                return false;
            }
        } else if (strcmp(name, "--cache") == 0 &&
                   options->cache_mode == NULL &&
                   (strcmp(value, "off") == 0 || strcmp(value, "on") == 0)) {
            options->cache_mode = value;
        } else if (strcmp(name, "--cache-url") == 0 &&
                   options->cache_url == NULL) {
            if (!cache_url_is_valid(value)) {
                return false;
            }
            options->cache_url = value;
        } else if (strcmp(name, "--ttl-seconds") == 0 &&
                   options->ttl_seconds == 0) {
            if (!positive_integer_parse(value, PRODUCT_ID_MAX,
                                        &options->ttl_seconds)) {
                return false;
            }
        } else if (strcmp(name, "--cache-timeout-ms") == 0 &&
                   options->cache_timeout_ms == 0) {
            if (!positive_integer_parse(value, PRODUCT_ID_MAX,
                                        &options->cache_timeout_ms)) {
                return false;
            }
        } else {
            return false;
        }
    }

    bool cache_options = options->cache_url != NULL ||
                         options->ttl_seconds != 0 ||
                         options->cache_timeout_ms != 0;
    if (options->ttl_seconds == 0) {
        options->ttl_seconds = CACHE_TTL_DEFAULT;
    }
    if (options->cache_timeout_ms == 0) {
        options->cache_timeout_ms = CACHE_TIMEOUT_DEFAULT;
    }
    if (options->cache_url == NULL) {
        options->cache_url = "http://127.0.0.1:8000";
    }

    return options->database != NULL &&
           ((options->seed != 0 && options->port == 0 &&
             options->cache_mode == NULL && !cache_options) ||
            (options->seed == 0 && options->port != 0 &&
             options->cache_mode != NULL && !has_count));
}

static bool positive_integer_parse(const char *text, int maximum, int *value)
{
    return integer_parse(text, strlen(text), maximum, value) && *value > 0;
}

static bool cache_url_is_valid(const char *url)
{
    if (strncmp(url, "http://", 7) != 0 || strcmp(mg_url_uri(url), "/") != 0 ||
        strpbrk(url, "\r\n\t @?#\\") != NULL) {
        return false;
    }
    const char *authority = url + 7;
    const char *end = strchr(authority, '/');
    if (end == NULL) {
        end = authority + strlen(authority);
    }
    const char *port = memchr(authority, ':', (size_t)(end - authority));
    const char *host_end = port == NULL ? end : port;
    if (host_end == authority) {
        return false;
    }
    for (const char *letter = authority; letter < host_end; letter++) {
        if (!((*letter >= 'a' && *letter <= 'z') ||
              (*letter >= 'A' && *letter <= 'Z') ||
              (*letter >= '0' && *letter <= '9') || *letter == '.' ||
              *letter == '-')) {
            return false;
        }
    }
    int number = 0;

    return port == NULL || (integer_parse(port + 1, (size_t)(end - port - 1),
                                          PORT_MAX, &number) &&
                            number > 0);
}

static bool integer_parse(const char *text, size_t text_len, int maximum,
                          int *value)
{
    if (text_len == 0) {
        return false;
    }
    int number = 0;
    for (size_t i = 0; i < text_len; i++) {
        int digit = text[i] - '0';
        if (digit < 0 || digit > 9 || number > (maximum - digit) / 10) {
            return false;
        }
        number = number * 10 + digit;
    }
    *value = number;

    return true;
}

static int server_run(struct demo *demo, int port)
{
    struct sigaction action = {0};
    action.sa_handler = stop_signal_handle;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    mg_log_set(MG_LL_ERROR);
    struct mg_mgr manager;
    mg_mgr_init(&manager);
    char url[LISTEN_URL_MAX];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    bool listening =
        mg_http_listen(&manager, url, http_event_handle, demo) != NULL;
    while (listening && !stop_requested) {
        mg_mgr_poll(&manager, POLL_INTERVAL_MS);
    }
    mg_mgr_free(&manager);

    return listening ? 0 : 1;
}

static void stop_signal_handle(int signum)
{
    (void)signum;
    stop_requested = 1;
}

static void http_event_handle(struct mg_connection *connection, int event,
                              void *event_data)
{
    if (event == MG_EV_HTTP_MSG && !connection->is_draining) {
        request_handle(connection, event_data);
    }
}

static void request_handle(struct mg_connection *connection,
                           struct mg_http_message *message)
{
    struct demo *demo = connection->fn_data;
    bool stats = mg_match(message->uri, mg_str("/stats"), NULL);
    bool list = mg_match(message->uri, mg_str("/products"), NULL);
    bool product =
        message->uri.len >= strlen("/products/") &&
        memcmp(message->uri.buf, "/products/", strlen("/products/")) == 0;
    if (!stats && !list && !product) {
        error_send(connection, 404, "route_not_found");
        return;
    }
    if (stats || list) {
        if (!method_is(message, "GET")) {
            method_not_allowed(connection, "GET");
        } else if (message->body.len != 0 ||
                   (stats && message->query.len != 0)) {
            error_send(connection, 400, "invalid_request");
        } else if (stats) {
            stats_send(connection, demo);
        } else {
            list_send(connection, demo, message->query);
        }
        return;
    }
    int product_id = 0;
    char decoded[32];
    int decoded_len = mg_url_decode(message->uri.buf + strlen("/products/"),
                                    message->uri.len - strlen("/products/"),
                                    decoded, sizeof decoded, 0);
    if (message->query.len != 0 || decoded_len < 0 ||
        !integer_parse(decoded, (size_t)decoded_len, PRODUCT_ID_MAX,
                       &product_id) ||
        product_id == 0) {
        error_send(connection, 400, "invalid_request");
    } else if (method_is(message, "GET")) {
        if (message->body.len != 0) {
            error_send(connection, 400, "invalid_request");
        } else {
            demo->product_gets++;
            product_send(connection, demo, product_id);
        }
    } else if (method_is(message, "PUT")) {
        product_update(connection, demo, product_id, message);
    } else if (method_is(message, "DELETE")) {
        if (message->body.len != 0) {
            error_send(connection, 400, "invalid_request");
        } else {
            product_delete(connection, demo, product_id);
        }
    } else {
        method_not_allowed(connection, "GET, PUT, DELETE");
    }
}

static bool method_is(const struct mg_http_message *message, const char *method)
{
    return mg_match(message->method, mg_str(method), NULL);
}

static void method_not_allowed(struct mg_connection *connection,
                               const char *allow)
{
    char headers[64];
    snprintf(headers, sizeof headers,
             "Content-Type: application/json\r\nAllow: %s\r\n", allow);
    mg_http_reply(connection, 405, headers,
                  "{\"error\":\"method_not_allowed\"}");
}

static void error_send(struct mg_connection *connection, int status,
                       const char *reason)
{
    mg_http_reply(connection, status, "Content-Type: application/json\r\n",
                  "{\"error\":\"%s\"}", reason);
}

static void stats_send(struct mg_connection *connection,
                       const struct demo *demo)
{
    const struct cache_client *cache = &demo->cache;
    mg_http_reply(
        connection, 200, "Content-Type: application/json\r\n",
        "{\"product_gets\":%llu,\"origin_queries\":%llu,"
        "\"origin_failures\":%llu,\"origin_changes\":%llu,"
        "\"invalidations\":%llu,\"cache_bypasses\":%llu,"
        "\"cache_gets\":%llu,\"cache_hits\":%llu,\"cache_misses\":%llu,"
        "\"cache_get_errors\":%llu,\"cache_errors\":%llu,"
        "\"cache_puts\":%llu,\"cache_put_failures\":%llu,"
        "\"cache_deletes\":%llu,\"cache_delete_failures\":%llu,"
        "\"cache_probes\":%llu,\"cache_probe_failures\":%llu,"
        "\"cache_recoveries\":%llu,\"origin\":\"%s\",\"cache_state\":\"%s\"}",
        (unsigned long long)demo->product_gets,
        (unsigned long long)demo->origin_queries,
        (unsigned long long)demo->origin_failures,
        (unsigned long long)demo->origin_changes,
        (unsigned long long)demo->invalidations,
        (unsigned long long)cache->bypasses, (unsigned long long)cache->gets,
        (unsigned long long)cache->hits, (unsigned long long)cache->misses,
        (unsigned long long)cache->get_errors,
        (unsigned long long)(cache->get_errors + cache->put_failures +
                             cache->delete_failures),
        (unsigned long long)cache->puts,
        (unsigned long long)cache->put_failures,
        (unsigned long long)cache->deletes,
        (unsigned long long)cache->delete_failures,
        (unsigned long long)cache->probes,
        (unsigned long long)cache->probe_failures,
        (unsigned long long)cache->recoveries, origin_name(),
        !cache->enabled ? "off"
        : cache->active ? "active"
                        : "suspended");
}

static void list_send(struct mg_connection *connection, struct demo *demo,
                      struct mg_str query)
{
    struct list_query list;
    if (!list_query_parse(query, &list)) {
        error_send(connection, 400, "invalid_request");
        return;
    }
    demo->product_gets++;
    cache_recover(demo);
    char key[LIST_KEY_MAX];
    list_key(demo, &list, key, sizeof key);
    char *cached = cache_client_get(&demo->cache, key);
    if (cached != NULL) {
        body_send(connection, cached);
        free(cached);
        return;
    }
    int status = 0;
    char *body = list_read(demo, &list, &status);
    if (body == NULL) {
        error_send(connection, status, "origin_unavailable");
        return;
    }
    cache_client_set(&demo->cache, key, body);
    body_send(connection, body);
    free(body);
}

static bool list_query_parse(struct mg_str query, struct list_query *list)
{
    list->q_len = 0;
    list->min_price = 0;
    list->max_price = PRICE_MAX;
    list->offset = 0;
    list->limit = LIST_LIMIT_DEFAULT;
    list->sort = LIST_SORT_ID;
    bool seen[6] = {false};
    size_t start = 0;
    while (start < query.len) {
        size_t end = start;
        while (end < query.len && query.buf[end] != '&') {
            end++;
        }
        if (!list_pair_apply(query.buf + start, end - start, list, seen)) {
            return false;
        }
        start = end + 1;
    }

    return list->min_price <= list->max_price;
}

static bool list_pair_apply(const char *pair, size_t pair_len,
                            struct list_query *list, bool *seen)
{
    static const char *names[] = {"q",    "min_price", "max_price",
                                  "sort", "offset",    "limit"};
    size_t equals = 0;
    while (equals < pair_len && pair[equals] != '=') {
        equals++;
    }
    char name[16];
    char value[LIST_VALUE_SIZE];
    size_t name_len = 0;
    size_t value_len = 0;
    if (equals == pair_len ||
        !list_value_decode(pair, equals, name, sizeof name, &name_len) ||
        !list_value_decode(pair + equals + 1, pair_len - equals - 1, value,
                           sizeof value, &value_len) ||
        strlen(name) != name_len || strlen(value) != value_len) {
        return false;
    }
    int field = -1;
    for (int i = 0; i < 6; i++) {
        if (strcmp(name, names[i]) == 0) {
            field = i;
        }
    }
    if (field < 0 || seen[field]) {
        return false;
    }
    seen[field] = true;

    switch (field) {
    case 0:
        if (value_len > NAME_MAX_BYTES ||
            (value_len != 0 &&
             !name_is_valid((const unsigned char *)value, (int)value_len))) {
            return false;
        }
        memcpy(list->q, value, value_len);
        list->q[value_len] = '\0';
        list->q_len = value_len;
        return true;
    case 1:
        return integer_parse(value, value_len, PRICE_MAX, &list->min_price);
    case 2:
        return integer_parse(value, value_len, PRICE_MAX, &list->max_price);
    case 3:
        if (strcmp(value, "id") == 0) {
            list->sort = LIST_SORT_ID;
        } else if (strcmp(value, "price_asc") == 0) {
            list->sort = LIST_SORT_PRICE_ASC;
        } else if (strcmp(value, "price_desc") == 0) {
            list->sort = LIST_SORT_PRICE_DESC;
        } else {
            return false;
        }
        return true;
    case 4:
        return integer_parse(value, value_len, LIST_OFFSET_MAX, &list->offset);
    default:
        return integer_parse(value, value_len, LIST_LIMIT_MAX, &list->limit) &&
               list->limit > 0;
    }
}

static bool list_value_decode(const char *text, size_t text_len, char *value,
                              size_t value_size, size_t *value_len)
{
    if (text_len >= value_size) {
        return false;
    }
    int decoded_len = mg_url_decode(text, text_len, value, value_size, 0);
    if (decoded_len < 0) {
        return false;
    }
    *value_len = (size_t)decoded_len;

    return true;
}

static void list_key(const struct demo *demo, const struct list_query *list,
                     char *key, size_t key_size)
{
    static const char *sort_names[] = {"id", "price_asc", "price_desc"};
    char encoded[LIST_VALUE_SIZE];
    size_t encoded_len =
        mg_url_encode(list->q, list->q_len, encoded, sizeof encoded);
    snprintf(key, key_size, "list:%llu:%.*s:%d:%d:%s:%d:%d",
             demo->list_generation, (int)encoded_len, encoded, list->min_price,
             list->max_price, sort_names[list->sort], list->offset,
             list->limit);
}

static char *list_read(struct demo *demo, const struct list_query *list,
                       int *status)
{
    struct text body = {0};
    struct items_context items = {&body, true};
    demo->origin_queries++;
    bool success =
        text_append(&body, "{\"items\":[") &&
        origin_list_read(demo->origin, list, items_append, &items) == 200 &&
        text_append(&body, "]}");
    *status = success ? 200 : 503;
    if (!success) {
        demo->origin_failures++;
        free(body.data);
        body.data = NULL;
    }

    return body.data;
}

static bool text_append(struct text *text, const char *piece)
{
    size_t piece_len = strlen(piece);
    if (text->len + piece_len + 1 > text->capacity) {
        size_t capacity = text->capacity == 0 ? 256 : text->capacity * 2;
        while (capacity < text->len + piece_len + 1) {
            capacity *= 2;
        }
        char *data = realloc(text->data, capacity);
        if (data == NULL) {
            return false;
        }
        text->data = data;
        text->capacity = capacity;
    }
    memcpy(text->data + text->len, piece, piece_len);
    text->len += piece_len;
    text->data[text->len] = '\0';

    return true;
}

static bool items_append(void *context, int product_id, const char *name,
                         size_t name_len, int price)
{
    struct items_context *items = context;
    if (name == NULL || product_id < 1 || product_id > PRODUCT_ID_MAX ||
        price < 0 || price > PRICE_MAX ||
        !name_is_valid((const unsigned char *)name, (int)name_len)) {
        return false;
    }
    char *item =
        mg_mprintf("%s{\"id\":%d,\"name\":%m,\"price\":%d}",
                   items->first ? "" : ",", product_id, MG_ESC(name), price);
    items->first = false;
    bool appended = item != NULL && text_append(items->body, item);
    free(item);

    return appended;
}

static void product_send(struct mg_connection *connection, struct demo *demo,
                         int product_id)
{
    cache_recover(demo);
    char key[64];
    snprintf(key, sizeof key, "detail:%d", product_id);
    char *cached = cache_client_get(&demo->cache, key);
    if (cached != NULL) {
        body_send(connection, cached);
        free(cached);
        return;
    }
    int status = 0;
    char *body = product_read(demo, product_id, &status);
    if (body == NULL) {
        error_send(connection, status,
                   status == 404 ? "product_not_found" : "origin_unavailable");
        return;
    }
    cache_client_set(&demo->cache, key, body);
    body_send(connection, body);
    free(body);
}

static void product_update(struct mg_connection *connection, struct demo *demo,
                           int product_id, struct mg_http_message *message)
{
    char name[NAME_MAX_BYTES + 1];
    size_t name_len = 0;
    int price = 0;
    if (message->body.len > UPDATE_BODY_MAX) {
        error_send(connection, 413, "body_too_large");
        return;
    }
    if (!content_type_is_json(message)) {
        error_send(connection, 415, "unsupported_media_type");
        return;
    }
    if (!update_parse(message->body, name, &name_len, &price)) {
        error_send(connection, 400, "invalid_request");
        return;
    }
    int status =
        origin_product_write(demo->origin, product_id, name, name_len, price);
    if (status == 503) {
        demo->origin_failures++;
    }
    if (status != 204) {
        error_send(connection, status,
                   status == 404 ? "product_not_found" : "origin_unavailable");
        return;
    }
    demo->origin_changes++;
    cache_invalidate(demo, product_id);
    mg_http_reply(connection, 204, "Content-Type: application/json\r\n", "");
}

static void product_delete(struct mg_connection *connection, struct demo *demo,
                           int product_id)
{
    int status = origin_product_remove(demo->origin, product_id);
    if (status == 503) {
        demo->origin_failures++;
    }
    if (status != 204) {
        error_send(connection, status,
                   status == 404 ? "product_not_found" : "origin_unavailable");
        return;
    }
    demo->origin_changes++;
    cache_invalidate(demo, product_id);
    mg_http_reply(connection, 204, "Content-Type: application/json\r\n", "");
}

static bool content_type_is_json(struct mg_http_message *message)
{
    struct mg_str *type = mg_http_get_header(message, "Content-Type");
    struct mg_str json_type = mg_str("application/json");

    return type != NULL && type->len >= json_type.len &&
           mg_strcasecmp(mg_str_n(type->buf, json_type.len), json_type) == 0;
}

static bool update_parse(struct mg_str body, char *name, size_t *name_len,
                         int *price)
{
    int root_len = 0;
    if (mg_json_get(body, "$", &root_len) != 0 ||
        (size_t)root_len != body.len) {
        return false;
    }
    bool has_name = false;
    bool has_price = false;
    struct mg_str key;
    struct mg_str value;
    size_t offset = 0;
    while ((offset = mg_json_next(body, offset, &key, &value)) != 0) {
        if (key.len == 6 && memcmp(key.buf, "\"name\"", 6) == 0 && !has_name) {
            has_name = true;
            size_t length =
                mg_json_unescape(body, "$.name", name, NAME_MAX_BYTES + 1);
            if (length == 0 ||
                !name_is_valid((const unsigned char *)name, (int)length)) {
                return false;
            }
            *name_len = length;
        } else if (key.len == 7 && memcmp(key.buf, "\"price\"", 7) == 0 &&
                   !has_price) {
            has_price = integer_parse(value.buf, value.len, PRICE_MAX, price);
            if (!has_price) {
                return false;
            }
        } else {
            return false;
        }
    }

    return has_name && has_price;
}

static void cache_recover(struct demo *demo)
{
    if (cache_client_recover(&demo->cache)) {
        demo->list_generation = 0;
    }
}

static void cache_invalidate(struct demo *demo, int product_id)
{
    char key[64];
    snprintf(key, sizeof key, "detail:%d", product_id);
    demo->list_generation++;
    if (demo->cache.enabled) {
        demo->invalidations++;
    }
    cache_client_delete(&demo->cache, key);
}

static void body_send(struct mg_connection *connection, const char *body)
{
    mg_http_reply(connection, 200, "Content-Type: application/json\r\n", "%s",
                  body);
}

static char *product_read(struct demo *demo, int product_id, int *status)
{
    struct text body = {0};
    struct items_context items = {&body, true};
    int count = 0;
    demo->origin_queries++;
    *status = origin_product_read(demo->origin, product_id, items_append,
                                  &items, &count);
    if (*status != 200) {
        free(body.data);
        body.data = NULL;
        if (*status == 503) {
            demo->origin_failures++;
        }
    }

    return body.data;
}

static bool name_is_valid(const unsigned char *name, int name_len)
{
    if (name == NULL || name_len < 1 || name_len > NAME_MAX_BYTES) {
        return false;
    }
    for (int i = 0; i < name_len;) {
        unsigned char first = name[i++];
        if (first == 0) {
            return false;
        }
        if (first < 0x80) {
            continue;
        }
        int following = first >= 0xc2 && first <= 0xdf   ? 1
                        : first >= 0xe0 && first <= 0xef ? 2
                        : first >= 0xf0 && first <= 0xf4 ? 3
                                                         : 0;
        if (following == 0 || i + following > name_len) {
            return false;
        }
        unsigned char second = name[i];
        if ((first == 0xe0 && second < 0xa0) ||
            (first == 0xed && second >= 0xa0) ||
            (first == 0xf0 && second < 0x90) ||
            (first == 0xf4 && second >= 0x90)) {
            return false;
        }
        for (int j = 0; j < following; j++) {
            if ((name[i++] & 0xc0) != 0x80) {
                return false;
            }
        }
    }

    return true;
}
