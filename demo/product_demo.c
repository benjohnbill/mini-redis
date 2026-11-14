#include "mongoose.h"
#include "cache_client.h"
#include <signal.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT_MAX 65535
#define PRODUCT_ID_MAX 2147483647
#define PRICE_MAX 1000000000
#define NAME_MAX_BYTES 120
#define PRODUCT_COUNT_DEFAULT 10000
#define SEED_MAX 2147483646
#define SEED_MULTIPLIER 48271
#define SQLITE_WAIT_MS 1000
#define POLL_INTERVAL_MS 10
#define LISTEN_URL_MAX 64
#define CACHE_TTL_DEFAULT 60
#define CACHE_TIMEOUT_DEFAULT 100

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
    sqlite3 *database;
    struct cache_client cache;
    uint64_t product_gets;
    uint64_t origin_queries;
    uint64_t origin_failures;
};

static volatile sig_atomic_t stop_requested;
static bool options_parse(int argc, char **argv, struct options *options);
static bool cache_url_is_valid(const char *url);
static bool integer_parse(const char *text, size_t text_len, int maximum,
                          int *value);
static bool database_open(const struct options *options, sqlite3 **database);
static bool database_seed(sqlite3 *database, const struct options *options);
static bool products_insert(sqlite3 *database, const struct options *options);
static int server_run(struct demo *demo, int port);
static void stop_signal_handle(int signum);
static void http_event_handle(struct mg_connection *connection, int event,
                              void *event_data);
static void request_handle(struct mg_connection *connection,
                           struct mg_http_message *message);
static void error_send(struct mg_connection *connection, int status,
                       const char *reason);
static void stats_send(struct mg_connection *connection,
                       const struct demo *demo);
static bool product_is_valid(sqlite3_stmt *statement);
static bool name_is_valid(const unsigned char *name, int name_len);
static void product_send(struct mg_connection *connection, struct demo *demo,
                         int product_id);

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
    if (!database_open(&options, &demo.database)) {
        sqlite3_close(demo.database);
        return 1;
    }
    int exit_code = 0;
    if (options.seed != 0) {
        exit_code = database_seed(demo.database, &options) ? 0 : 1;
    } else {
        bool initialized = cache_client_init(
            &demo.cache,
            strcmp(options.cache_mode, "on") == 0 ? options.cache_url : NULL,
            options.ttl_seconds, options.cache_timeout_ms);
        exit_code = initialized ? server_run(&demo, options.port) : 1;
        cache_client_destroy(&demo.cache);
    }
    sqlite3_close(demo.database);

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
            if (!integer_parse(value, strlen(value), PORT_MAX,
                               &options->port) ||
                options->port == 0) {
                return false;
            }
        } else if (strcmp(name, "--seed") == 0 && options->seed == 0) {
            if (!integer_parse(value, strlen(value), SEED_MAX,
                               &options->seed) ||
                options->seed == 0) {
                return false;
            }
        } else if (strcmp(name, "--count") == 0 && !has_count) {
            has_count = true;
            if (!integer_parse(value, strlen(value), PRODUCT_ID_MAX,
                               &options->count) ||
                options->count == 0) {
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
            if (!integer_parse(value, strlen(value), PRODUCT_ID_MAX,
                               &options->ttl_seconds) ||
                options->ttl_seconds == 0) {
                return false;
            }
        } else if (strcmp(name, "--cache-timeout-ms") == 0 &&
                   options->cache_timeout_ms == 0) {
            if (!integer_parse(value, strlen(value), PRODUCT_ID_MAX,
                               &options->cache_timeout_ms) ||
                options->cache_timeout_ms == 0) {
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

static bool database_open(const struct options *options, sqlite3 **database)
{
    int flags = SQLITE_OPEN_READWRITE;
    if (options->seed != 0) {
        flags |= SQLITE_OPEN_CREATE;
    }
    if (sqlite3_open_v2(options->database, database, flags, NULL) !=
        SQLITE_OK) {
        fprintf(stderr, "cache-demo: cannot open origin\n");
        return false;
    }
    sqlite3_busy_timeout(*database, SQLITE_WAIT_MS);
    if (options->seed == 0 &&
        sqlite3_exec(*database, "SELECT id,name,price FROM products LIMIT 0",
                     NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "cache-demo: origin schema unavailable\n");
        return false;
    }

    return true;
}

static bool database_seed(sqlite3 *database, const struct options *options)
{
    const char *schema =
        "BEGIN IMMEDIATE;"
        "CREATE TABLE IF NOT EXISTS products ("
        "id INTEGER PRIMARY KEY CHECK(id BETWEEN 1 AND 2147483647),"
        "name TEXT NOT NULL CHECK(length(CAST(name AS BLOB)) BETWEEN 1 AND 120"
        " AND instr(name,char(0))=0),"
        "price INTEGER NOT NULL CHECK(typeof(price)='integer' AND "
        "price BETWEEN 0 AND 1000000000));"
        "CREATE INDEX IF NOT EXISTS products_price_id ON products(price,id);"
        "DELETE FROM products;";
    bool success =
        sqlite3_exec(database, schema, NULL, NULL, NULL) == SQLITE_OK &&
        products_insert(database, options) &&
        sqlite3_exec(database, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    if (!success) {
        sqlite3_exec(database, "ROLLBACK", NULL, NULL, NULL);
        fprintf(stderr, "cache-demo: seed failed\n");
    }

    return success;
}

static bool products_insert(sqlite3 *database, const struct options *options)
{
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, "INSERT INTO products VALUES(?,?,?)", -1,
                           &statement, NULL) != SQLITE_OK) {
        return false;
    }
    int64_t state = options->seed;
    bool success = true;
    for (int64_t i = 1; i <= options->count && success; i++) {
        char name[64];
        snprintf(name, sizeof name, "상품 %lld", (long long)i);
        state = state * SEED_MULTIPLIER % PRODUCT_ID_MAX;
        success = sqlite3_bind_int64(statement, 1, i) == SQLITE_OK &&
                  sqlite3_bind_text(statement, 2, name, -1, SQLITE_TRANSIENT) ==
                      SQLITE_OK &&
                  sqlite3_bind_int64(statement, 3, state % (PRICE_MAX + 1)) ==
                      SQLITE_OK &&
                  sqlite3_step(statement) == SQLITE_DONE;
        sqlite3_reset(statement);
    }
    sqlite3_finalize(statement);

    return success;
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
    bool product =
        message->uri.len >= strlen("/products/") &&
        memcmp(message->uri.buf, "/products/", strlen("/products/")) == 0;
    if (!stats && !product) {
        error_send(connection, 404, "route_not_found");
        return;
    }
    if (!mg_match(message->method, mg_str("GET"), NULL)) {
        mg_http_reply(connection, 405,
                      "Content-Type: application/json\r\nAllow: GET\r\n",
                      "{\"error\":\"method_not_allowed\"}");
        return;
    }
    int product_id = 0;
    char decoded[32];
    int decoded_len =
        product ? mg_url_decode(message->uri.buf + strlen("/products/"),
                                message->uri.len - strlen("/products/"),
                                decoded, sizeof decoded, 0)
                : 0;
    if (message->query.buf != NULL || message->body.len != 0 ||
        (product && (decoded_len < 0 ||
                     !integer_parse(decoded, (size_t)decoded_len,
                                    PRODUCT_ID_MAX, &product_id) ||
                     product_id == 0))) {
        error_send(connection, 400, "invalid_request");
        return;
    }
    if (stats) {
        stats_send(connection, demo);
    } else {
        demo->product_gets++;
        product_send(connection, demo, product_id);
    }
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
        "\"origin_failures\":%llu,\"cache_bypasses\":%llu,"
        "\"cache_gets\":%llu,\"cache_hits\":%llu,\"cache_misses\":%llu,"
        "\"cache_get_errors\":%llu,\"cache_errors\":%llu,"
        "\"cache_puts\":%llu,\"cache_put_failures\":%llu,"
        "\"cache_state\":\"%s\"}",
        (unsigned long long)demo->product_gets,
        (unsigned long long)demo->origin_queries,
        (unsigned long long)demo->origin_failures,
        (unsigned long long)cache->bypasses, (unsigned long long)cache->gets,
        (unsigned long long)cache->hits, (unsigned long long)cache->misses,
        (unsigned long long)cache->get_errors,
        (unsigned long long)(cache->get_errors + cache->put_failures),
        (unsigned long long)cache->puts,
        (unsigned long long)cache->put_failures,
        !cache->enabled ? "off"
        : cache->active ? "active"
                        : "suspended");
}

static void product_send(struct mg_connection *connection, struct demo *demo,
                         int product_id)
{
    char key[64];
    snprintf(key, sizeof key, "detail:%d", product_id);
    char *cached = cache_client_get(&demo->cache, key);
    if (cached != NULL) {
        mg_http_reply(connection, 200, "Content-Type: application/json\r\n",
                      "%s", cached);
        free(cached);
        return;
    }
    demo->origin_queries++;
    sqlite3_stmt *statement = NULL;
    int result = sqlite3_prepare_v2(
        demo->database, "SELECT id,name,price FROM products WHERE id=?", -1,
        &statement, NULL);
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int(statement, 1, product_id);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_step(statement);
    }
    if (result == SQLITE_ROW && !product_is_valid(statement)) {
        result = SQLITE_MISMATCH;
    }
    if (result == SQLITE_ROW) {
        char *body =
            mg_mprintf("{\"id\":%d,\"name\":%m,\"price\":%lld}", product_id,
                       MG_ESC((const char *)sqlite3_column_text(statement, 1)),
                       (long long)sqlite3_column_int64(statement, 2));
        if (body == NULL) {
            error_send(connection, 503, "origin_unavailable");
            demo->origin_failures++;
        } else {
            cache_client_set(&demo->cache, key, body);
            mg_http_reply(connection, 200, "Content-Type: application/json\r\n",
                          "%s", body);
            free(body);
        }
    } else if (result == SQLITE_DONE) {
        error_send(connection, 404, "product_not_found");
    } else {
        demo->origin_failures++;
        error_send(connection, 503, "origin_unavailable");
    }
    sqlite3_finalize(statement);
}

static bool product_is_valid(sqlite3_stmt *statement)
{
    sqlite3_int64 product_id = sqlite3_column_int64(statement, 0);
    sqlite3_int64 price = sqlite3_column_int64(statement, 2);

    return sqlite3_column_type(statement, 0) == SQLITE_INTEGER &&
           product_id >= 1 && product_id <= PRODUCT_ID_MAX &&
           sqlite3_column_type(statement, 1) == SQLITE_TEXT &&
           name_is_valid(sqlite3_column_text(statement, 1),
                         sqlite3_column_bytes(statement, 1)) &&
           sqlite3_column_type(statement, 2) == SQLITE_INTEGER && price >= 0 &&
           price <= PRICE_MAX;
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
