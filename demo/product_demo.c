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
#define PRODUCT_COUNT_DEFAULT 10000
#define SEED_MAX 2147483646
#define POLL_INTERVAL_MS 10
#define LISTEN_URL_MAX 64
#define CACHE_URL_DEFAULT "http://127.0.0.1:8000"
#define CACHE_TTL_DEFAULT 60
#define CACHE_TIMEOUT_DEFAULT 100
#define HTTP_SCHEME "http://"
#define PRODUCT_PATH_PREFIX "/products/"
#define PRODUCT_ID_TEXT_MAX 32
#define ALLOW_HEADERS_MAX 64
#define DETAIL_KEY_MAX 64
#define LIST_LIMIT_DEFAULT 20
#define LIST_LIMIT_MAX 100
#define LIST_OFFSET_MAX 1000000
#define LIST_KEY_MAX 512
#define LIST_FIELD_NAME_MAX 16
#define UPDATE_BODY_MAX 4096
#define TEXT_CAPACITY_INITIAL 256
/* URL 인코딩은 바이트 하나를 %XX 세 글자로 늘립니다. */
#define URL_ENCODED_BYTES_MAX 3
#define LIST_VALUE_RAW_MAX (NAME_MAX_BYTES * URL_ENCODED_BYTES_MAX)
#define LIST_VALUE_SIZE (LIST_VALUE_RAW_MAX + 2)
/* 대기 목록에 쌓는 상세 조회의 상한입니다. 차면 쌓인 것을 먼저 처리합니다.
   mongoose는 응답 전까지 연결마다 다음 요청을 해석하지 않으므로, 보통은 열린
   연결 수를 넘지 않습니다. */
#define PENDING_MAX 256
/* mini-redis의 POST /mget이 한 번에 받는 key의 상한입니다(docs/api.md). */
#define MGET_KEYS_MAX 64

/* 목록 query string이 받는 필드입니다. 순서는 list_field_find의 이름 표와
   같습니다. */
enum list_field {
    LIST_FIELD_SEARCH,
    LIST_FIELD_MIN_PRICE,
    LIST_FIELD_MAX_PRICE,
    LIST_FIELD_SORT,
    LIST_FIELD_OFFSET,
    LIST_FIELD_LIMIT,
    LIST_FIELD_COUNT
};

/* STAGE_TRACE가 요청 하나에서 따로 재는 구간입니다. 순서는 기록 줄의 칸
   순서와 같습니다. */
enum stage { STAGE_CACHE_GET, STAGE_ORIGIN, STAGE_CACHE_PUT, STAGE_COUNT };

#ifdef STAGE_TRACE
#include <time.h>

#define STAGE_BATCH_MAX 1024
/* mg_mgr_poll 한 번에 기록하는 묶음 끝 저장의 상한입니다. 대기 목록은
   PENDING_MAX개이고 MGET_KEYS_MAX개씩 나눠 처리하므로 저장은 몇 번뿐입니다. */
#define STAGE_STORE_MAX 16
#define STAGE_NOT_ENTERED (-1.0)
#define MS_PER_SECOND 1000.0
#define NS_PER_MS 1000000.0

/* 요청 하나의 시각(ms)과 구간 시간(ms)입니다. 지나지 않은 구간은
   STAGE_NOT_ENTERED이고, 응답을 소켓에 다 쓰기 전이면 sent_ms가 0입니다. */
struct stage_record {
    const char *kind;
    unsigned long connection_id;
    unsigned long long batch_number;
    size_t batch_size;
    double started_ms;
    double finished_ms;
    double waited_ms;
    double stage_ms[STAGE_COUNT];
    double held_ms;
    double sent_ms;
    /* POST /mget으로 묶여 캐시를 물었는지와 그 묶음의 key 수, 그 POST /mget의
       번호(기록 파일 안에서 1부터)입니다. 같은 번호의 요청들은 왕복 한 번을
       함께 썼습니다. */
    bool is_bundled;
    size_t mget_keys;
    unsigned long long mget_number;
    /* 원본에서 읽은 body를 응답 뒤에 묶음 끝에서 캐시에 저장하는 요청입니다.
       캐시 PUT 칸은 그 저장이 끝날 때 채웁니다. */
    bool is_store_deferred;
};

/* 묶음 끝의 캐시 저장 왕복 하나입니다. key가 2개 이상이면 POST /mset, 1개면
   PUT입니다. */
struct stage_store {
    unsigned long long batch_number;
    size_t key_count;
    double stored_ms;
};

/* mg_mgr_poll 한 번이 처리한 요청들과 그 사이의 묶음 끝 저장 왕복들입니다.
   request_count는 기록하지 못한 요청까지 셉니다. */
struct stage_batch {
    size_t request_count;
    struct stage_record records[STAGE_BATCH_MAX];
    size_t store_count;
    struct stage_store stores[STAGE_STORE_MAX];
};

/* 데모 백엔드는 스레드 하나로 요청을 처리하므로 기록 상태도 하나입니다.
   batch_started_ms가 0이면 이번 묶음의 첫 이벤트가 아직 오지 않았습니다.
   묶음의 응답은 대개 다음 mg_mgr_poll에서 소켓에 쓰이므로, 앞 묶음(previous)의
   줄은 지금 묶음이 끝날 때 적습니다. */
struct stage_trace {
    FILE *file;
    unsigned long long batch_number;
    double batch_started_ms;
    double stage_started_ms;
    /* 대기 목록의 묶음 하나를 처리하기 시작한 시각과, 그 묶음의 POST /mget
       왕복 시간, key 수, 번호입니다. mget_count는 지금까지 부른 POST /mget의
       수입니다. */
    double chunk_started_ms;
    double chunk_mget_ms;
    size_t chunk_keys;
    unsigned long long chunk_mget_number;
    unsigned long long mget_count;
    /* 대기 묶음의 첫 요청이 batch.records에 들어갈 자리와, 묶음 끝의 캐시 저장을
       시작한 시각입니다. */
    size_t chunk_first_record;
    double store_started_ms;
    unsigned long long dropped_count;
    /* 한 묶음에서 STAGE_STORE_MAX를 넘어 store 줄로 남기지 못한 저장의 수입니다. */
    unsigned long long dropped_store_count;
    struct stage_record current;
    struct stage_batch batch;
    struct stage_batch previous;
};

static struct stage_trace stage_trace_state;
#endif

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

/* 대기 목록의 상세 조회 하나입니다. mongoose는 닫힌 연결을 해제하므로 연결은
   포인터가 아니라 id로 기억합니다. */
struct pending_detail {
    unsigned long connection_id;
    int product_id;
    /* 요청에 Connection: close가 있어 응답 뒤에 연결을 닫습니다. */
    bool is_last;
};

/* 묶음 하나에서 mini-redis에 물을 key입니다. 같은 상품을 원하는 요청들은 key
   하나를 함께 씁니다. key_index[i]는 묶음의 i번째 요청이 쓰는 key의 번호입니다. */
struct pending_keys {
    char names[MGET_KEYS_MAX][DETAIL_KEY_MAX];
    const char *list[MGET_KEYS_MAX];
    int product_ids[MGET_KEYS_MAX];
    size_t key_index[MGET_KEYS_MAX];
    size_t count;
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
    struct mg_mgr *manager;
    struct pending_detail pending[PENDING_MAX];
    size_t pending_count;
};

static volatile sig_atomic_t stop_requested;
static bool options_parse(int argc, char **argv, struct options *options);
static bool option_apply(const char *name, const char *value,
                         struct options *options, bool *has_count);
static bool positive_integer_parse(const char *text, int maximum, int *value);
static bool integer_parse(const char *text, size_t text_len, int maximum,
                          int *value);
static bool cache_url_is_valid(const char *url);
static bool host_is_valid(const char *host, const char *host_end);
static bool options_are_valid(const struct options *options, bool has_count);
static void options_defaults_fill(struct options *options);
static int server_run(struct demo *demo, int port);
static void stop_signals_install(void);
static void stop_signal_handle(int signum);
static void http_event_handle(struct mg_connection *connection, int event,
                              void *event_data);
static void message_handle(struct mg_connection *connection,
                           struct mg_http_message *message);
static bool detail_get_is(const struct mg_http_message *message);
static void detail_get_receive(struct mg_connection *connection,
                               struct demo *demo,
                               struct mg_http_message *message);
static void request_respond(struct mg_connection *connection,
                            struct mg_http_message *message);
static void pending_run(struct demo *demo);
static void pending_live_keep(struct demo *demo);
static struct mg_connection *connection_find(struct mg_mgr *manager,
                                             unsigned long connection_id);
static void pending_chunk_run(struct demo *demo,
                              const struct pending_detail *chunk,
                              size_t chunk_len);
static void pending_keys_build(struct pending_keys *keys,
                               const struct pending_detail *chunk,
                               size_t chunk_len);
static void pending_replies_share(struct demo *demo,
                                  const struct pending_detail *chunk,
                                  size_t chunk_len,
                                  const struct pending_keys *keys,
                                  char **bodies);
static void pending_stores_run(struct demo *demo,
                               const struct pending_keys *keys,
                               char *const *bodies, const bool *needs_store);
static void pending_reply_by_get(struct demo *demo,
                                 const struct pending_detail *pending);
static void pending_reply_from(struct demo *demo,
                               const struct pending_detail *pending,
                               const char *key, char *cached, bool is_bundled);
static void pending_reply_finish(struct mg_connection *connection,
                                 const struct pending_detail *pending);
static void request_handle(struct mg_connection *connection,
                           struct mg_http_message *message);
static void response_flush(struct mg_connection *connection);
static void stats_request_handle(struct mg_connection *connection,
                                 struct demo *demo,
                                 struct mg_http_message *message);
static bool method_is(const struct mg_http_message *message,
                      const char *method);
static void method_not_allowed_send(struct mg_connection *connection,
                                    const char *allow);
static void error_send(struct mg_connection *connection, int status,
                       const char *reason);
static void stats_send(struct mg_connection *connection,
                       const struct demo *demo);
static void list_request_handle(struct mg_connection *connection,
                                struct demo *demo,
                                struct mg_http_message *message);
static void list_send(struct mg_connection *connection, struct demo *demo,
                      struct mg_str query);
static bool list_query_parse(struct mg_str query, struct list_query *list);
static void list_query_init(struct list_query *list);
static bool list_pair_apply(const char *pair, size_t pair_len,
                            struct list_query *list, bool *seen);
static bool list_value_decode(const char *text, size_t text_len, char *value,
                              size_t value_size, size_t *value_len);
static bool list_field_find(const char *name, enum list_field *field);
static bool list_field_apply(enum list_field field, const char *value,
                             size_t value_len, struct list_query *list);
static bool list_search_text_set(const char *value, size_t value_len,
                                 struct list_query *list);
static bool name_is_valid(const unsigned char *name, int name_len);
static bool list_sort_parse(const char *value, enum list_sort *sort);
static void cache_recover(struct demo *demo);
static void list_key_build(const struct demo *demo,
                           const struct list_query *list, char *key,
                           size_t key_size);
static void body_send(struct mg_connection *connection, const char *body);
static char *list_body_create(struct demo *demo, const struct list_query *list,
                              int *status);
static bool text_append(struct text *text, const char *piece);
static bool items_append(void *context, int product_id, const char *name,
                         size_t name_len, int price);
static bool path_is_product(struct mg_str uri);
static void product_request_handle(struct mg_connection *connection,
                                   struct demo *demo,
                                   struct mg_http_message *message);
static bool product_id_parse(const struct mg_http_message *message,
                             int *product_id);
static void product_send(struct mg_connection *connection, struct demo *demo,
                         int product_id);
static void product_reply(struct mg_connection *connection, struct demo *demo,
                          int product_id, const char *key, char *cached);
static char *product_body_load(struct demo *demo, int product_id,
                               const char *key, char *cached, int *status);
static char *product_body_from_origin(struct demo *demo, int product_id,
                                      int *status);
static void detail_key_build(int product_id, char *key, size_t key_size);
static char *product_body_create(struct demo *demo, int product_id,
                                 int *status);
static void origin_error_send(struct mg_connection *connection, int status);
static void product_update(struct mg_connection *connection, struct demo *demo,
                           int product_id, struct mg_http_message *message);
static bool content_type_is_json(struct mg_http_message *message);
static bool update_parse(struct mg_str body, char *name, size_t *name_len,
                         int *price);
static bool json_key_is(struct mg_str key, const char *quoted_name);
static void product_change_reply(struct mg_connection *connection,
                                 struct demo *demo, int product_id, int status);
static void cache_invalidate(struct demo *demo, int product_id);
static void product_delete(struct mg_connection *connection, struct demo *demo,
                           int product_id);
static void stage_trace_open(void);
static void stage_trace_batch_begin(void);
static void stage_trace_request_begin(const struct mg_connection *connection,
                                      const struct mg_http_message *message);
static void stage_trace_stage_begin(enum stage stage);
static void stage_trace_stage_end(enum stage stage);
static bool stage_trace_cache_begin(const struct demo *demo, enum stage stage);
static void stage_trace_cache_end(bool is_traced, enum stage stage);
static void stage_trace_chunk_begin(void);
static void stage_trace_chunk_mget_end(size_t key_count);
static void stage_trace_deferred_begin(const struct mg_connection *connection,
                                       bool is_bundled);
static void stage_trace_store_defer(void);
static bool stage_trace_store_begin(const struct demo *demo);
static void stage_trace_store_end(bool is_traced, size_t key_count);
static void stage_trace_request_end(void);
static void stage_trace_sent(const struct mg_connection *connection);
static void stage_trace_batch_end(void);
static void stage_trace_close(void);
#ifdef STAGE_TRACE
static double stage_trace_now(void);
static void stage_record_start(const char *kind, unsigned long connection_id,
                               double started_ms);
static const char *stage_kind_find(const struct mg_http_message *message);
static void stage_batch_sent(struct stage_batch *batch,
                             unsigned long connection_id, double now);
static void stage_batch_write(const struct stage_batch *batch);
static void stage_record_write(const struct stage_record *record);
#endif

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
    demo.origin = origin_create(options.database, options.seed != 0);
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

/* 이름과 값의 쌍만 받습니다. 기본값은 조합을 검사한 뒤에 채웁니다. 캐시 옵션을
   주었는지는 기본값을 채우기 전에만 알 수 있기 때문입니다. */
static bool options_parse(int argc, char **argv, struct options *options)
{
    bool has_count = false;
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc ||
            !option_apply(argv[i], argv[i + 1], options, &has_count)) {
            return false;
        }
    }
    bool is_valid = options_are_valid(options, has_count);
    options_defaults_fill(options);

    return is_valid;
}

/* 옵션 하나를 options에 넣습니다. 모르는 옵션, 두 번 온 옵션, 잘못된 값은
   false입니다. */
static bool option_apply(const char *name, const char *value,
                         struct options *options, bool *has_count)
{
    if (strcmp(name, "--db") == 0 && options->database == NULL &&
        value[0] != '\0') {
        options->database = value;
        return true;
    }
    if (strcmp(name, "--port") == 0 && options->port == 0) {
        return positive_integer_parse(value, PORT_MAX, &options->port);
    }
    if (strcmp(name, "--seed") == 0 && options->seed == 0) {
        return positive_integer_parse(value, SEED_MAX, &options->seed);
    }
    if (strcmp(name, "--count") == 0 && !*has_count) {
        *has_count = true;
        return positive_integer_parse(value, PRODUCT_ID_MAX, &options->count);
    }
    if (strcmp(name, "--cache") == 0 && options->cache_mode == NULL &&
        (strcmp(value, "off") == 0 || strcmp(value, "on") == 0)) {
        options->cache_mode = value;
        return true;
    }
    if (strcmp(name, "--cache-url") == 0 && options->cache_url == NULL &&
        cache_url_is_valid(value)) {
        options->cache_url = value;
        return true;
    }
    if (strcmp(name, "--ttl-seconds") == 0 && options->ttl_seconds == 0) {
        return positive_integer_parse(value, PRODUCT_ID_MAX,
                                      &options->ttl_seconds);
    }
    if (strcmp(name, "--cache-timeout-ms") == 0 &&
        options->cache_timeout_ms == 0) {
        return positive_integer_parse(value, PRODUCT_ID_MAX,
                                      &options->cache_timeout_ms);
    }

    return false;
}

static bool positive_integer_parse(const char *text, int maximum, int *value)
{
    return integer_parse(text, strlen(text), maximum, value) && *value > 0;
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

static bool cache_url_is_valid(const char *url)
{
    size_t scheme_len = strlen(HTTP_SCHEME);
    if (strncmp(url, HTTP_SCHEME, scheme_len) != 0 ||
        strcmp(mg_url_uri(url), "/") != 0 ||
        strpbrk(url, "\r\n\t @?#\\") != NULL) {
        return false;
    }
    const char *authority = url + scheme_len;
    const char *end = strchr(authority, '/');
    if (end == NULL) {
        end = authority + strlen(authority);
    }
    const char *port = memchr(authority, ':', (size_t)(end - authority));
    const char *host_end = port == NULL ? end : port;
    if (!host_is_valid(authority, host_end)) {
        return false;
    }
    int number = 0;

    return port == NULL || (integer_parse(port + 1, (size_t)(end - port - 1),
                                          PORT_MAX, &number) &&
                            number > 0);
}

/* host는 비어 있지 않고, 영문자, 숫자, 점, 하이픈만 씁니다. */
static bool host_is_valid(const char *host, const char *host_end)
{
    if (host_end == host) {
        return false;
    }
    for (const char *letter = host; letter < host_end; letter++) {
        if (!((*letter >= 'a' && *letter <= 'z') ||
              (*letter >= 'A' && *letter <= 'Z') ||
              (*letter >= '0' && *letter <= '9') || *letter == '.' ||
              *letter == '-')) {
            return false;
        }
    }

    return true;
}

/* 데이터를 채우는 실행(--seed)과 서버 실행(--port)은 옵션을 섞지 않습니다. */
static bool options_are_valid(const struct options *options, bool has_count)
{
    bool has_cache_options = options->cache_url != NULL ||
                             options->ttl_seconds != 0 ||
                             options->cache_timeout_ms != 0;
    bool is_seed_run = options->seed != 0 && options->port == 0 &&
                       options->cache_mode == NULL && !has_cache_options;
    bool is_server_run = options->seed == 0 && options->port != 0 &&
                         options->cache_mode != NULL && !has_count;

    return options->database != NULL && (is_seed_run || is_server_run);
}

static void options_defaults_fill(struct options *options)
{
    if (options->ttl_seconds == 0) {
        options->ttl_seconds = CACHE_TTL_DEFAULT;
    }
    if (options->cache_timeout_ms == 0) {
        options->cache_timeout_ms = CACHE_TIMEOUT_DEFAULT;
    }
    if (options->cache_url == NULL) {
        options->cache_url = CACHE_URL_DEFAULT;
    }
}

static int server_run(struct demo *demo, int port)
{
    stop_signals_install();
    mg_log_set(MG_LL_ERROR);
    struct mg_mgr manager;
    mg_mgr_init(&manager);
    char url[LISTEN_URL_MAX];
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    bool listening =
        mg_http_listen(&manager, url, http_event_handle, demo) != NULL;
    demo->manager = &manager;
    stage_trace_open();
    while (listening && !stop_requested) {
        mg_mgr_poll(&manager, POLL_INTERVAL_MS);
        /* 이번 poll에 쌓인 상세 조회를 묶어 처리합니다. */
        pending_run(demo);
        stage_trace_batch_end();
    }
    stage_trace_close();
    mg_mgr_free(&manager);

    return listening ? 0 : 1;
}

static void stop_signals_install(void)
{
    struct sigaction action = {0};
    action.sa_handler = stop_signal_handle;
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
}

static void stop_signal_handle(int signum)
{
    (void)signum;
    stop_requested = 1;
}

static void http_event_handle(struct mg_connection *connection, int event,
                              void *event_data)
{
    /* mg_mgr_poll은 기다림이 끝나면 연결마다 MG_EV_POLL을 먼저 보내고 그다음
       요청을 읽습니다. 그래서 첫 MG_EV_POLL이 묶음의 시작입니다. MG_EV_WRITE는
       mongoose가 send()로 send 버퍼를 소켓에 쓴 직후에 옵니다. */
    if (event == MG_EV_POLL) {
        stage_trace_batch_begin();
    } else if (event == MG_EV_WRITE) {
        stage_trace_sent(connection);
    } else if (event == MG_EV_HTTP_MSG && !connection->is_draining) {
        message_handle(connection, event_data);
    }
}

/* 상세 조회는 대기 목록에 넣고, mg_mgr_poll이 돌아온 뒤 한꺼번에 처리합니다.
   다른 요청은 쌓인 상세 조회를 먼저 처리한 뒤 바로 처리합니다. 그래서 같은
   poll에서 수정 앞에 온 상세 조회는 옛 값을, 뒤에 온 상세 조회는 새 값을
   받습니다. */
static void message_handle(struct mg_connection *connection,
                           struct mg_http_message *message)
{
    struct demo *demo = connection->fn_data;
    if (detail_get_is(message)) {
        detail_get_receive(connection, demo, message);
        return;
    }
    pending_run(demo);
    request_respond(connection, message);
}

static bool detail_get_is(const struct mg_http_message *message)
{
    return path_is_product(message->uri) && method_is(message, "GET");
}

/* 입력 검사는 받을 때 해서, 잘못된 상세 조회는 대기 목록에 넣지 않고 바로
   400으로 답합니다. mongoose는 응답하기 전까지 이 연결의 다음 요청을 해석하지
   않으므로(is_resp), 연결 하나의 요청 순서는 그대로입니다. */
static void detail_get_receive(struct mg_connection *connection,
                               struct demo *demo,
                               struct mg_http_message *message)
{
    int product_id = 0;
    if (message->body.len != 0 || !product_id_parse(message, &product_id)) {
        request_respond(connection, message);
        return;
    }
    demo->product_gets++;
    if (demo->pending_count == PENDING_MAX) {
        pending_run(demo);
    }
    struct mg_str *close_header = mg_http_get_header(message, "Connection");
    struct pending_detail *pending = &demo->pending[demo->pending_count++];
    pending->connection_id = connection->id;
    pending->product_id = product_id;
    pending->is_last = close_header != NULL &&
                       mg_strcasecmp(*close_header, mg_str("close")) == 0;
}

/* 요청을 지금 처리하고 응답을 바로 보냅니다. */
static void request_respond(struct mg_connection *connection,
                            struct mg_http_message *message)
{
    stage_trace_request_begin(connection, message);
    request_handle(connection, message);
    stage_trace_request_end();
    response_flush(connection);
}

/* 대기 목록의 상세 조회를 받은 순서대로 처리합니다. 대기 중에 닫힌 연결의
   요청은 버리고, 남은 요청을 MGET_KEYS_MAX개씩 나눠 묶음마다 캐시를 묻습니다. */
static void pending_run(struct demo *demo)
{
    pending_live_keep(demo);
    for (size_t start = 0; start < demo->pending_count;
         start += MGET_KEYS_MAX) {
        size_t left = demo->pending_count - start;
        pending_chunk_run(demo, demo->pending + start,
                          left < MGET_KEYS_MAX ? left : MGET_KEYS_MAX);
    }
    demo->pending_count = 0;
}

static void pending_live_keep(struct demo *demo)
{
    size_t kept_count = 0;
    for (size_t i = 0; i < demo->pending_count; i++) {
        if (connection_find(demo->manager, demo->pending[i].connection_id) !=
            NULL) {
            demo->pending[kept_count++] = demo->pending[i];
        }
    }
    demo->pending_count = kept_count;
}

/* id로 연결을 찾습니다. 없거나 닫히는 중인 연결은 NULL입니다. */
static struct mg_connection *connection_find(struct mg_mgr *manager,
                                             unsigned long connection_id)
{
    for (struct mg_connection *connection = manager->conns; connection != NULL;
         connection = connection->next) {
        if (connection->id == connection_id) {
            return connection->is_closing || connection->is_draining
                       ? NULL
                       : connection;
        }
    }

    return NULL;
}

/* 묶음 하나(요청 1개부터 MGET_KEYS_MAX개까지)를 처리합니다. 요청이 2개 이상이면
   캐시를 POST /mget 한 번으로 묻고, 1개면 기존 GET 경로로 처리합니다. 같은
   상품을 원하는 요청이 여럿이면 key는 한 번만 묻습니다. mini-redis가 응답이 너무
   크다고 거절하면 요청마다 GET으로 다시 묻습니다. 다른 실패는 cache_client_mget이
   GET 실패와 같게 캐시를 중단하므로, 요청마다 원본으로 갑니다. */
static void pending_chunk_run(struct demo *demo,
                              const struct pending_detail *chunk,
                              size_t chunk_len)
{
    cache_recover(demo);
    stage_trace_chunk_begin();
    if (chunk_len == 1) {
        pending_reply_by_get(demo, chunk);
        return;
    }
    struct pending_keys keys;
    pending_keys_build(&keys, chunk, chunk_len);
    char *bodies[MGET_KEYS_MAX];
    enum cache_mget_result result =
        cache_client_mget(&demo->cache, keys.list, keys.count, bodies);
    stage_trace_chunk_mget_end(keys.count);
    if (result == CACHE_MGET_DONE) {
        pending_replies_share(demo, chunk, chunk_len, &keys, bodies);
        return;
    }
    /* 캐시가 답하지 않았으면 bodies는 모두 NULL입니다. */
    for (size_t i = 0; i < chunk_len; i++) {
        if (result == CACHE_MGET_TOO_LARGE) {
            pending_reply_by_get(demo, &chunk[i]);
        } else {
            pending_reply_from(demo, &chunk[i], keys.names[keys.key_index[i]],
                               NULL, result != CACHE_MGET_BYPASSED);
        }
    }
}

/* 묶음의 요청마다 key를 정하고, 같은 상품의 요청은 같은 key 하나를 쓰게 합니다.
   key는 처음 나온 순서대로 쌓습니다. */
static void pending_keys_build(struct pending_keys *keys,
                               const struct pending_detail *chunk,
                               size_t chunk_len)
{
    keys->count = 0;
    for (size_t i = 0; i < chunk_len; i++) {
        size_t found = 0;
        while (found < keys->count &&
               keys->product_ids[found] != chunk[i].product_id) {
            found++;
        }
        if (found == keys->count) {
            keys->product_ids[found] = chunk[i].product_id;
            detail_key_build(chunk[i].product_id, keys->names[found],
                             DETAIL_KEY_MAX);
            keys->list[found] = keys->names[found];
            keys->count++;
        }
        keys->key_index[i] = found;
    }
}

/* POST /mget이 답한 묶음의 요청들에 응답합니다. key마다 body를 한 번만 정하고,
   그 key를 원하는 요청 모두에 같은 body를 보냅니다. 캐시에 없던 key는 처음
   응답하는 요청이 원본을 읽습니다. 하나씩 처리했다면 뒤 요청은 앞 요청이 저장한
   값을 캐시에서 받았을 것이므로, 원본 조회와 저장을 되풀이하지 않습니다.
   원본에서 읽은 body는 응답을 먼저 보낸 뒤, 묶음의 응답을 모두 보내고 나서
   저장합니다. 응답이 캐시 저장을 기다리지 않게 하기 위해서입니다. 저장은 이
   함수 안에서 끝나므로, 다음 수정·삭제 요청보다 먼저 끝납니다. 그 순서가
   뒤집히면 수정이 무효화한 key에 옛 값을 다시 쓸 수 있습니다. bodies는 이
   함수가 해제합니다. */
static void pending_replies_share(struct demo *demo,
                                  const struct pending_detail *chunk,
                                  size_t chunk_len,
                                  const struct pending_keys *keys,
                                  char **bodies)
{
    bool is_loaded[MGET_KEYS_MAX] = {false};
    bool needs_store[MGET_KEYS_MAX] = {false};
    int statuses[MGET_KEYS_MAX] = {0};
    for (size_t i = 0; i < chunk_len; i++) {
        struct mg_connection *connection =
            connection_find(demo->manager, chunk[i].connection_id);
        if (connection == NULL) {
            continue;
        }
        size_t key_index = keys->key_index[i];
        stage_trace_deferred_begin(connection, true);
        if (!is_loaded[key_index] && bodies[key_index] == NULL) {
            bodies[key_index] = product_body_from_origin(
                demo, chunk[i].product_id, &statuses[key_index]);
            needs_store[key_index] = bodies[key_index] != NULL;
            if (needs_store[key_index]) {
                stage_trace_store_defer();
            }
        }
        is_loaded[key_index] = true;
        if (bodies[key_index] == NULL) {
            origin_error_send(connection, statuses[key_index]);
        } else {
            body_send(connection, bodies[key_index]);
        }
        pending_reply_finish(connection, &chunk[i]);
    }
    pending_stores_run(demo, keys, bodies, needs_store);
    for (size_t key_index = 0; key_index < keys->count; key_index++) {
        free(bodies[key_index]);
    }
}

/* 원본에서 읽은 body들을 캐시에 저장합니다. 묶음의 응답을 모두 보낸 뒤에
   부릅니다. 저장할 key가 2개 이상이면 POST /mset 한 번으로, 1개면 PUT으로
   저장합니다. */
static void pending_stores_run(struct demo *demo,
                               const struct pending_keys *keys,
                               char *const *bodies, const bool *needs_store)
{
    const char *store_keys[MGET_KEYS_MAX];
    const char *store_bodies[MGET_KEYS_MAX];
    size_t store_count = 0;
    for (size_t key_index = 0; key_index < keys->count; key_index++) {
        if (needs_store[key_index]) {
            store_keys[store_count] = keys->names[key_index];
            store_bodies[store_count] = bodies[key_index];
            store_count++;
        }
    }
    if (store_count == 0) {
        return;
    }
    bool is_traced = stage_trace_store_begin(demo);
    if (store_count == 1) {
        cache_client_set(&demo->cache, store_keys[0], store_bodies[0]);
    } else {
        cache_client_mset(&demo->cache, store_keys, store_bodies, store_count);
    }
    stage_trace_store_end(is_traced, store_count);
}

static void pending_reply_by_get(struct demo *demo,
                                 const struct pending_detail *pending)
{
    struct mg_connection *connection =
        connection_find(demo->manager, pending->connection_id);
    if (connection == NULL) {
        return;
    }
    stage_trace_deferred_begin(connection, false);
    product_send(connection, demo, pending->product_id);
    pending_reply_finish(connection, pending);
}

/* cached는 POST /mget이 돌려준 body이거나 NULL(원본 조회)이며, 이 함수가
   해제합니다. */
static void pending_reply_from(struct demo *demo,
                               const struct pending_detail *pending,
                               const char *key, char *cached, bool is_bundled)
{
    struct mg_connection *connection =
        connection_find(demo->manager, pending->connection_id);
    if (connection == NULL) {
        free(cached);
        return;
    }
    stage_trace_deferred_begin(connection, is_bundled);
    product_reply(connection, demo, pending->product_id, key, cached);
    pending_reply_finish(connection, pending);
}

static void pending_reply_finish(struct mg_connection *connection,
                                 const struct pending_detail *pending)
{
    stage_trace_request_end();
    response_flush(connection);
    if (pending->is_last) {
        connection->is_draining = 1;
    }
}

static void request_handle(struct mg_connection *connection,
                           struct mg_http_message *message)
{
    struct demo *demo = connection->fn_data;
    if (mg_match(message->uri, mg_str("/stats"), NULL)) {
        stats_request_handle(connection, demo, message);
    } else if (mg_match(message->uri, mg_str("/products"), NULL)) {
        list_request_handle(connection, demo, message);
    } else if (path_is_product(message->uri)) {
        product_request_handle(connection, demo, message);
    } else {
        error_send(connection, 404, "route_not_found");
    }
}

/* mongoose는 handler가 send 버퍼에 쌓은 응답을 다음 mg_mgr_poll에서 씁니다.
   요청이 한꺼번에 몰리면 한 poll이 그 요청을 모두 처리한 뒤에야 응답이 나가므로,
   처리한 직후 소켓에 바로 씁니다. 다 쓰지 못한 바이트와 오류는 버퍼에 남겨
   mongoose가 다음 mg_mgr_poll에서 원래대로 다루게 합니다. */
static void response_flush(struct mg_connection *connection)
{
    if (connection->send.len == 0 || connection->is_closing) {
        return;
    }
    long sent_len =
        mg_io_send(connection, connection->send.buf, connection->send.len);
    if (sent_len > 0) {
        mg_iobuf_del(&connection->send, 0, (size_t)sent_len);
        stage_trace_sent(connection);
    }
}

static void stats_request_handle(struct mg_connection *connection,
                                 struct demo *demo,
                                 struct mg_http_message *message)
{
    if (!method_is(message, "GET")) {
        method_not_allowed_send(connection, "GET");
    } else if (message->body.len != 0 || message->query.len != 0) {
        error_send(connection, 400, "invalid_request");
    } else {
        stats_send(connection, demo);
    }
}

static bool method_is(const struct mg_http_message *message, const char *method)
{
    return mg_match(message->method, mg_str(method), NULL);
}

static void method_not_allowed_send(struct mg_connection *connection,
                                    const char *allow)
{
    char headers[ALLOW_HEADERS_MAX];
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
        "\"cache_recoveries\":%llu,\"cache_mgets\":%llu,"
        "\"cache_mget_keys\":%llu,\"cache_msets\":%llu,"
        "\"cache_mset_keys\":%llu,\"origin\":\"%s\",\"cache_state\":\"%s\"}",
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
        (unsigned long long)cache->recoveries, (unsigned long long)cache->mgets,
        (unsigned long long)cache->mget_keys, (unsigned long long)cache->msets,
        (unsigned long long)cache->mset_keys, origin_name(),
        !cache->enabled ? "off"
        : cache->active ? "active"
                        : "suspended");
}

static void list_request_handle(struct mg_connection *connection,
                                struct demo *demo,
                                struct mg_http_message *message)
{
    if (!method_is(message, "GET")) {
        method_not_allowed_send(connection, "GET");
    } else if (message->body.len != 0) {
        error_send(connection, 400, "invalid_request");
    } else {
        list_send(connection, demo, message->query);
    }
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
    list_key_build(demo, &list, key, sizeof key);
    bool is_get_traced = stage_trace_cache_begin(demo, STAGE_CACHE_GET);
    char *cached = cache_client_get(&demo->cache, key);
    stage_trace_cache_end(is_get_traced, STAGE_CACHE_GET);
    if (cached != NULL) {
        body_send(connection, cached);
        free(cached);
        return;
    }
    int status = 0;
    stage_trace_stage_begin(STAGE_ORIGIN);
    char *body = list_body_create(demo, &list, &status);
    stage_trace_stage_end(STAGE_ORIGIN);
    if (body == NULL) {
        error_send(connection, status, "origin_unavailable");
        return;
    }
    bool is_put_traced = stage_trace_cache_begin(demo, STAGE_CACHE_PUT);
    cache_client_set(&demo->cache, key, body);
    stage_trace_cache_end(is_put_traced, STAGE_CACHE_PUT);
    body_send(connection, body);
    free(body);
}

static bool list_query_parse(struct mg_str query, struct list_query *list)
{
    list_query_init(list);
    bool seen[LIST_FIELD_COUNT] = {false};
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

static void list_query_init(struct list_query *list)
{
    list->search_text_len = 0;
    list->min_price = 0;
    list->max_price = PRICE_MAX;
    list->offset = 0;
    list->limit = LIST_LIMIT_DEFAULT;
    list->sort = LIST_SORT_ID;
}

static bool list_pair_apply(const char *pair, size_t pair_len,
                            struct list_query *list, bool *seen)
{
    size_t separator_index = 0;
    while (separator_index < pair_len && pair[separator_index] != '=') {
        separator_index++;
    }
    char name[LIST_FIELD_NAME_MAX];
    char value[LIST_VALUE_SIZE];
    size_t name_len = 0;
    size_t value_len = 0;
    enum list_field field = LIST_FIELD_SEARCH;
    if (separator_index == pair_len ||
        !list_value_decode(pair, separator_index, name, sizeof name,
                           &name_len) ||
        !list_value_decode(pair + separator_index + 1,
                           pair_len - separator_index - 1, value, sizeof value,
                           &value_len) ||
        strlen(name) != name_len || strlen(value) != value_len ||
        !list_field_find(name, &field) || seen[field]) {
        return false;
    }
    seen[field] = true;

    return list_field_apply(field, value, value_len, list);
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

static bool list_field_find(const char *name, enum list_field *field)
{
    static const char *names[LIST_FIELD_COUNT] = {
        "q", "min_price", "max_price", "sort", "offset", "limit"};
    for (int i = 0; i < LIST_FIELD_COUNT; i++) {
        if (strcmp(name, names[i]) == 0) {
            *field = (enum list_field)i;
            return true;
        }
    }

    return false;
}

static bool list_field_apply(enum list_field field, const char *value,
                             size_t value_len, struct list_query *list)
{
    switch (field) {
    case LIST_FIELD_SEARCH:
        return list_search_text_set(value, value_len, list);
    case LIST_FIELD_MIN_PRICE:
        return integer_parse(value, value_len, PRICE_MAX, &list->min_price);
    case LIST_FIELD_MAX_PRICE:
        return integer_parse(value, value_len, PRICE_MAX, &list->max_price);
    case LIST_FIELD_SORT:
        return list_sort_parse(value, &list->sort);
    case LIST_FIELD_OFFSET:
        return integer_parse(value, value_len, LIST_OFFSET_MAX, &list->offset);
    case LIST_FIELD_LIMIT:
        return integer_parse(value, value_len, LIST_LIMIT_MAX, &list->limit) &&
               list->limit > 0;
    default:
        return false;
    }
}

/* 빈 검색어는 검색하지 않는다는 뜻입니다. */
static bool list_search_text_set(const char *value, size_t value_len,
                                 struct list_query *list)
{
    if (value_len > NAME_MAX_BYTES ||
        (value_len != 0 &&
         !name_is_valid((const unsigned char *)value, (int)value_len))) {
        return false;
    }
    memcpy(list->search_text, value, value_len);
    list->search_text[value_len] = '\0';
    list->search_text_len = value_len;

    return true;
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

static bool list_sort_parse(const char *value, enum list_sort *sort)
{
    if (strcmp(value, "id") == 0) {
        *sort = LIST_SORT_ID;
    } else if (strcmp(value, "price_asc") == 0) {
        *sort = LIST_SORT_PRICE_ASC;
    } else if (strcmp(value, "price_desc") == 0) {
        *sort = LIST_SORT_PRICE_DESC;
    } else {
        return false;
    }

    return true;
}

static void cache_recover(struct demo *demo)
{
    if (cache_client_recover(&demo->cache)) {
        demo->list_generation = 0;
    }
}

static void list_key_build(const struct demo *demo,
                           const struct list_query *list, char *key,
                           size_t key_size)
{
    static const char *sort_names[] = {"id", "price_asc", "price_desc"};
    char encoded[LIST_VALUE_SIZE];
    size_t encoded_len = mg_url_encode(list->search_text, list->search_text_len,
                                       encoded, sizeof encoded);
    snprintf(key, key_size, "list:%llu:%.*s:%d:%d:%s:%d:%d",
             demo->list_generation, (int)encoded_len, encoded, list->min_price,
             list->max_price, sort_names[list->sort], list->offset,
             list->limit);
}

static void body_send(struct mg_connection *connection, const char *body)
{
    mg_http_reply(connection, 200, "Content-Type: application/json\r\n", "%s",
                  body);
}

static char *list_body_create(struct demo *demo, const struct list_query *list,
                              int *status)
{
    struct text body = {0};
    struct items_context items = {&body, true};
    demo->origin_queries++;
    bool success =
        text_append(&body, "{\"items\":[") &&
        origin_list_get(demo->origin, list, items_append, &items) == 200 &&
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
        size_t capacity =
            text->capacity == 0 ? TEXT_CAPACITY_INITIAL : text->capacity * 2;
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

static bool path_is_product(struct mg_str uri)
{
    size_t prefix_len = strlen(PRODUCT_PATH_PREFIX);

    return uri.len >= prefix_len &&
           memcmp(uri.buf, PRODUCT_PATH_PREFIX, prefix_len) == 0;
}

static void product_request_handle(struct mg_connection *connection,
                                   struct demo *demo,
                                   struct mg_http_message *message)
{
    int product_id = 0;
    if (!product_id_parse(message, &product_id)) {
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
        method_not_allowed_send(connection, "GET, PUT, DELETE");
    }
}

/* 경로의 id는 1 이상의 10진수이고, query string은 받지 않습니다. */
static bool product_id_parse(const struct mg_http_message *message,
                             int *product_id)
{
    size_t prefix_len = strlen(PRODUCT_PATH_PREFIX);
    char decoded[PRODUCT_ID_TEXT_MAX];
    int decoded_len = mg_url_decode(message->uri.buf + prefix_len,
                                    message->uri.len - prefix_len, decoded,
                                    sizeof decoded, 0);

    return message->query.len == 0 && decoded_len >= 0 &&
           integer_parse(decoded, (size_t)decoded_len, PRODUCT_ID_MAX,
                         product_id) &&
           *product_id != 0;
}

static void product_send(struct mg_connection *connection, struct demo *demo,
                         int product_id)
{
    cache_recover(demo);
    char key[DETAIL_KEY_MAX];
    detail_key_build(product_id, key, sizeof key);
    bool is_get_traced = stage_trace_cache_begin(demo, STAGE_CACHE_GET);
    char *cached = cache_client_get(&demo->cache, key);
    stage_trace_cache_end(is_get_traced, STAGE_CACHE_GET);
    product_reply(connection, demo, product_id, key, cached);
}

/* 캐시에서 받은 body가 있으면 그대로 보내고, 없으면 원본에서 만들어 캐시에
   저장한 뒤 보냅니다. cached는 이 함수가 해제합니다. */
static void product_reply(struct mg_connection *connection, struct demo *demo,
                          int product_id, const char *key, char *cached)
{
    int status = 0;
    char *body = product_body_load(demo, product_id, key, cached, &status);
    if (body == NULL) {
        origin_error_send(connection, status);
        return;
    }
    body_send(connection, body);
    free(body);
}

/* 캐시에서 받은 body가 있으면 그대로 돌려주고, 없으면 원본에서 만들어 캐시에
   저장한 뒤 돌려줍니다. 원본에서 만들지 못하면 NULL을 돌려주고 status에 응답
   코드를 적습니다. 돌려준 body는 부른 쪽이 해제합니다. */
static char *product_body_load(struct demo *demo, int product_id,
                               const char *key, char *cached, int *status)
{
    if (cached != NULL) {
        return cached;
    }
    char *body = product_body_from_origin(demo, product_id, status);
    if (body == NULL) {
        return NULL;
    }
    bool is_put_traced = stage_trace_cache_begin(demo, STAGE_CACHE_PUT);
    cache_client_set(&demo->cache, key, body);
    stage_trace_cache_end(is_put_traced, STAGE_CACHE_PUT);

    return body;
}

/* 원본에서 상품 body를 만듭니다. 캐시에는 저장하지 않습니다. 만들지 못하면
   NULL을 돌려주고 status에 응답 코드를 적습니다. */
static char *product_body_from_origin(struct demo *demo, int product_id,
                                      int *status)
{
    stage_trace_stage_begin(STAGE_ORIGIN);
    char *body = product_body_create(demo, product_id, status);
    stage_trace_stage_end(STAGE_ORIGIN);

    return body;
}

static void detail_key_build(int product_id, char *key, size_t key_size)
{
    snprintf(key, key_size, "detail:%d", product_id);
}

static char *product_body_create(struct demo *demo, int product_id, int *status)
{
    struct text body = {0};
    struct items_context items = {&body, true};
    int count = 0;
    demo->origin_queries++;
    *status = origin_product_get(demo->origin, product_id, items_append, &items,
                                 &count);
    if (*status != 200) {
        free(body.data);
        body.data = NULL;
        if (*status == 503) {
            demo->origin_failures++;
        }
    }

    return body.data;
}

/* 원본이 돌려준 404와 503을 오류 이유로 바꿔 보냅니다. */
static void origin_error_send(struct mg_connection *connection, int status)
{
    error_send(connection, status,
               status == 404 ? "product_not_found" : "origin_unavailable");
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
        origin_product_set(demo->origin, product_id, name, name_len, price);
    product_change_reply(connection, demo, product_id, status);
}

static bool content_type_is_json(struct mg_http_message *message)
{
    struct mg_str *type = mg_http_get_header(message, "Content-Type");
    struct mg_str json_type = mg_str("application/json");

    if (type == NULL || type->len < json_type.len ||
        mg_strcasecmp(mg_str_n(type->buf, json_type.len), json_type) != 0) {
        return false;
    }

    return type->len == json_type.len || type->buf[json_type.len] == ';' ||
           type->buf[json_type.len] == ' ' || type->buf[json_type.len] == '\t';
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
        if (json_key_is(key, "\"name\"") && !has_name) {
            has_name = true;
            size_t length =
                mg_json_unescape(body, "$.name", name, NAME_MAX_BYTES + 1);
            if (length == 0 ||
                !name_is_valid((const unsigned char *)name, (int)length)) {
                return false;
            }
            *name_len = length;
        } else if (json_key_is(key, "\"price\"") && !has_price) {
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

/* mg_json_next가 돌려주는 key는 큰따옴표를 포함합니다. */
static bool json_key_is(struct mg_str key, const char *quoted_name)
{
    size_t name_len = strlen(quoted_name);

    return key.len == name_len && memcmp(key.buf, quoted_name, name_len) == 0;
}

/* 수정과 삭제가 원본에 반영되면 캐시를 무효화하고 204로 답합니다. */
static void product_change_reply(struct mg_connection *connection,
                                 struct demo *demo, int product_id, int status)
{
    if (status == 503) {
        demo->origin_failures++;
    }
    if (status != 204) {
        origin_error_send(connection, status);
        return;
    }
    demo->origin_changes++;
    cache_invalidate(demo, product_id);
    mg_http_reply(connection, 204, "Content-Type: application/json\r\n", "");
}

static void cache_invalidate(struct demo *demo, int product_id)
{
    char key[DETAIL_KEY_MAX];
    detail_key_build(product_id, key, sizeof key);
    demo->list_generation++;
    if (demo->cache.enabled) {
        demo->invalidations++;
    }
    cache_client_delete(&demo->cache, key);
}

static void product_delete(struct mg_connection *connection, struct demo *demo,
                           int product_id)
{
    int status = origin_product_delete(demo->origin, product_id);
    product_change_reply(connection, demo, product_id, status);
}

/* 측정용 데모(make cache-stage-bench)만 STAGE_TRACE를 켭니다. 요청 하나마다
   아래 칸을 STAGE_TRACE_FILE이 가리키는 파일에 한 줄로 적습니다. 시간은 ms이고,
   지나지 않은 구간과 알 수 없는 값은 "-"입니다.

     stage <묶음 번호> <종류> <대기> <처리> <캐시 GET> <원본 조회> <캐시 PUT>
           <붙잡힘> <실제 붙잡힘> <mget 묶임> <mget key 수> <mget 번호>
           <묶음 요청 수>

   상세 조회 묶음 끝에서 캐시에 저장한 왕복은 하나마다 아래 줄을 그 묶음의 요청
   줄들 뒤에 적습니다. 저장할 key가 2개 이상이면 mset, 1개면 put입니다.

     store <묶음 번호> <put|mset> <key 수> <저장 시간>

   그 저장을 맡긴 요청들의 캐시 PUT 칸에도 같은 저장 시간을 적으므로, 캐시 PUT
   칸의 개수는 저장 왕복 수가 아닙니다. store 줄은 상세 조회 묶음 끝의 저장만
   담습니다. 상세 요청이 하나라 단건 GET 경로를 탄 요청의 PUT과 목록·검색의
   PUT은 요청 줄의 캐시 PUT 칸에만 있습니다. 그래서 전체 PUT과 mset의 수는 store
   줄이 아니라 /stats의 cache_puts와 cache_msets로 셉니다. 한 묶음에서
   STAGE_STORE_MAX를 넘은 저장은 store 줄을 남기지 못하고, 파일 끝에
   "dropped_stores <수>"로 셉니다.

   묶음은 mg_mgr_poll 한 번이 처리한 요청들이고, 묶음 번호는 몇 번째
   mg_mgr_poll인지입니다(1부터). 대기는 묶음이 시작된 뒤 처리가 시작되기까지,
   처리는 처리 시작부터 응답을 만들 때까지입니다. 대기 목록에 넣은 상세 조회는
   그 요청이 든 대기 묶음(POST /mget 하나)을 처리하기 시작한 때가 처리
   시작입니다. mget 묶임은 POST /mget으로 캐시를 물었으면 1, 아니면 0이고,
   묶였으면 캐시 GET 칸은 그 POST /mget의 왕복 시간, mget key 수는 그 key
   수, mget 번호는 그 POST /mget이 이 파일에서 몇 번째인지입니다(1부터). 같은
   번호의 요청들은 왕복 한 번을 함께 썼습니다. 묶이지 않았으면 mget key 수와
   mget 번호는 "-"입니다. 붙잡힘은 처리가 끝난 뒤 mg_mgr_poll이
   돌아오기까지이고, 실제 붙잡힘은 처리가 끝난 뒤 응답을 소켓에 다 쓰기까지
   입니다. 응답은 response_flush가 처리 직후에 쓰고, 다 쓰지 못한 나머지는
   mongoose가 다음 mg_mgr_poll에서 쓰므로, 다음 묶음이 끝날 때까지 다 쓰지 못한
   응답의 실제 붙잡힘은 "-"입니다.
   STAGE_TRACE_FILE이 없으면 적지 않습니다. make verify의 빌드에서는 아무것도
   하지 않습니다. */
static void stage_trace_open(void)
{
#ifdef STAGE_TRACE
    stage_trace_state.batch_number = 1;
    const char *path = getenv("STAGE_TRACE_FILE");
    if (path == NULL) {
        return;
    }
    stage_trace_state.file = fopen(path, "w");
    if (stage_trace_state.file == NULL) {
        fprintf(stderr, "stage trace: cannot open %s\n", path);
    }
#endif
}

static void stage_trace_batch_begin(void)
{
#ifdef STAGE_TRACE
    if (stage_trace_state.file != NULL &&
        stage_trace_state.batch_started_ms == 0) {
        stage_trace_state.batch_started_ms = stage_trace_now();
    }
#endif
}

static void stage_trace_request_begin(const struct mg_connection *connection,
                                      const struct mg_http_message *message)
{
#ifdef STAGE_TRACE
    if (stage_trace_state.file == NULL) {
        return;
    }
    stage_record_start(stage_kind_find(message), connection->id,
                       stage_trace_now());
#else
    (void)connection;
    (void)message;
#endif
}

/* 대기 묶음 하나를 처리하기 시작한 시각을 적습니다. POST /mget 왕복도 이때부터
   잽니다. */
static void stage_trace_chunk_begin(void)
{
#ifdef STAGE_TRACE
    if (stage_trace_state.file != NULL) {
        stage_trace_state.chunk_started_ms = stage_trace_now();
        stage_trace_state.chunk_mget_ms = STAGE_NOT_ENTERED;
        stage_trace_state.chunk_keys = 0;
        stage_trace_state.chunk_mget_number = 0;
        stage_trace_state.chunk_first_record =
            stage_trace_state.batch.request_count;
    }
#endif
}

static void stage_trace_chunk_mget_end(size_t key_count)
{
#ifdef STAGE_TRACE
    if (stage_trace_state.file != NULL) {
        stage_trace_state.chunk_mget_ms =
            stage_trace_now() - stage_trace_state.chunk_started_ms;
        stage_trace_state.chunk_keys = key_count;
        stage_trace_state.chunk_mget_number = ++stage_trace_state.mget_count;
    }
#else
    (void)key_count;
#endif
}

/* 대기 목록에서 꺼낸 상세 조회의 기록을 시작합니다. 처리 시작은 그 대기 묶음을
   처리하기 시작한 시각입니다. */
static void stage_trace_deferred_begin(const struct mg_connection *connection,
                                       bool is_bundled)
{
#ifdef STAGE_TRACE
    if (stage_trace_state.file == NULL) {
        return;
    }
    stage_record_start("detail", connection->id,
                       stage_trace_state.chunk_started_ms);
    struct stage_record *record = &stage_trace_state.current;
    if (is_bundled) {
        record->is_bundled = true;
        record->mget_keys = stage_trace_state.chunk_keys;
        record->mget_number = stage_trace_state.chunk_mget_number;
        record->stage_ms[STAGE_CACHE_GET] = stage_trace_state.chunk_mget_ms;
    }
#else
    (void)connection;
    (void)is_bundled;
#endif
}

static void stage_trace_stage_begin(enum stage stage)
{
    (void)stage;
#ifdef STAGE_TRACE
    stage_trace_state.stage_started_ms = stage_trace_now();
#endif
}

static void stage_trace_stage_end(enum stage stage)
{
#ifdef STAGE_TRACE
    stage_trace_state.current.stage_ms[stage] =
        stage_trace_now() - stage_trace_state.stage_started_ms;
#else
    (void)stage;
#endif
}

/* 캐시 GET과 PUT은 캐시에 실제로 요청을 보낼 때만 잽니다. 캐시가 꺼졌거나
   중단되면 cache_client_get과 cache_client_set이 요청 없이 돌아오므로, 그
   호출까지 세면 왕복하지 않은 요청이 0 ms로 섞입니다. 호출 도중에 캐시가
   중단될 수 있으므로, 잴지는 호출 전에 정하고 그 값을 끝에 넘깁니다. */
static bool stage_trace_cache_begin(const struct demo *demo, enum stage stage)
{
    if (!demo->cache.active) {
        return false;
    }
    stage_trace_stage_begin(stage);

    return true;
}

static void stage_trace_cache_end(bool is_traced, enum stage stage)
{
    if (is_traced) {
        stage_trace_stage_end(stage);
    }
}

/* 지금 요청이 원본에서 읽은 body를 응답 뒤 묶음 끝에서 저장한다고 적습니다. */
static void stage_trace_store_defer(void)
{
#ifdef STAGE_TRACE
    stage_trace_state.current.is_store_deferred = true;
#endif
}

/* 묶음 끝의 캐시 저장을 재기 시작합니다. 캐시에 실제로 요청을 보낼 때만 잽니다
   (stage_trace_cache_begin과 같은 이유). 돌려준 값을 stage_trace_store_end에
   넘깁니다. */
static bool stage_trace_store_begin(const struct demo *demo)
{
    if (!demo->cache.active) {
        return false;
    }
#ifdef STAGE_TRACE
    stage_trace_state.store_started_ms = stage_trace_now();
#endif

    return true;
}

/* 상세 조회 묶음 끝의 캐시 저장 왕복 하나를 store 줄로 남기고, 그 시간을 이 대기
   묶음에서
   저장을 맡긴 요청들의 캐시 PUT 칸에도 적습니다. 그 요청들의 기록은 응답을 보낼
   때 이미 batch.records에 들어갔습니다. 요청들은 저장 왕복 하나를 함께
   기다렸으므로 모두 같은 시간을 적고, 그래서 캐시 PUT 칸의 개수는 저장 왕복 수가
   아닙니다. 저장은 응답을 만든 뒤라 이 시간은 요청의 처리 시간에 들지 않습니다. */
static void stage_trace_store_end(bool is_traced, size_t key_count)
{
#ifdef STAGE_TRACE
    if (!is_traced || stage_trace_state.file == NULL) {
        return;
    }
    double stored_ms = stage_trace_now() - stage_trace_state.store_started_ms;
    struct stage_batch *batch = &stage_trace_state.batch;
    if (batch->store_count < STAGE_STORE_MAX) {
        batch->stores[batch->store_count++] =
            (struct stage_store){.batch_number = stage_trace_state.batch_number,
                                 .key_count = key_count,
                                 .stored_ms = stored_ms};
    } else {
        stage_trace_state.dropped_store_count++;
    }
    size_t end = batch->request_count < STAGE_BATCH_MAX ? batch->request_count
                                                        : STAGE_BATCH_MAX;
    for (size_t i = stage_trace_state.chunk_first_record; i < end; i++) {
        if (batch->records[i].is_store_deferred) {
            batch->records[i].stage_ms[STAGE_CACHE_PUT] = stored_ms;
        }
    }
#else
    (void)is_traced;
    (void)key_count;
#endif
}

/* 붙잡힘은 묶음이 끝나야 알 수 있으므로, 요청의 기록을 묶음 끝까지 모읍니다. */
static void stage_trace_request_end(void)
{
#ifdef STAGE_TRACE
    if (stage_trace_state.file == NULL) {
        return;
    }
    struct stage_batch *batch = &stage_trace_state.batch;
    struct stage_record *record = &stage_trace_state.current;
    record->finished_ms = stage_trace_now();
    record->waited_ms = record->started_ms - stage_trace_state.batch_started_ms;
    if (batch->request_count < STAGE_BATCH_MAX) {
        batch->records[batch->request_count] = *record;
    } else {
        stage_trace_state.dropped_count++;
    }
    batch->request_count++;
#endif
}

/* 연결의 send 버퍼가 비었으면, 그 연결에서 아직 보내지 않은 응답을 모두 지금
   보낸 것으로 적습니다. response_flush와 MG_EV_WRITE가 부릅니다. */
static void stage_trace_sent(const struct mg_connection *connection)
{
#ifdef STAGE_TRACE
    if (stage_trace_state.file == NULL || connection->send.len != 0) {
        return;
    }
    double now = stage_trace_now();
    stage_batch_sent(&stage_trace_state.batch, connection->id, now);
    stage_batch_sent(&stage_trace_state.previous, connection->id, now);
#else
    (void)connection;
#endif
}

/* 앞 묶음의 줄을 적고, 지금 묶음의 붙잡힘과 요청 수를 정해 앞 묶음으로
   옮깁니다. */
static void stage_trace_batch_end(void)
{
#ifdef STAGE_TRACE
    if (stage_trace_state.file == NULL) {
        return;
    }
    double now = stage_trace_now();
    stage_batch_write(&stage_trace_state.previous);
    struct stage_batch *batch = &stage_trace_state.batch;
    struct stage_batch *previous = &stage_trace_state.previous;
    previous->request_count = batch->request_count;
    for (size_t i = 0; i < batch->request_count && i < STAGE_BATCH_MAX; i++) {
        batch->records[i].held_ms = now - batch->records[i].finished_ms;
        batch->records[i].batch_size = batch->request_count;
        previous->records[i] = batch->records[i];
    }
    previous->store_count = batch->store_count;
    for (size_t i = 0; i < batch->store_count; i++) {
        previous->stores[i] = batch->stores[i];
    }
    batch->request_count = 0;
    batch->store_count = 0;
    stage_trace_state.batch_started_ms = 0;
    stage_trace_state.batch_number++;
#endif
}

static void stage_trace_close(void)
{
#ifdef STAGE_TRACE
    if (stage_trace_state.file == NULL) {
        return;
    }
    stage_batch_write(&stage_trace_state.previous);
    if (stage_trace_state.dropped_count > 0) {
        fprintf(stage_trace_state.file, "dropped %llu\n",
                stage_trace_state.dropped_count);
    }
    if (stage_trace_state.dropped_store_count > 0) {
        fprintf(stage_trace_state.file, "dropped_stores %llu\n",
                stage_trace_state.dropped_store_count);
    }
    fclose(stage_trace_state.file);
    stage_trace_state.file = NULL;
#endif
}

#ifdef STAGE_TRACE
/* mg_millis는 ms 단위로 잘리므로 구간 시간은 단조 시계로 직접 잽니다. */
static double stage_trace_now(void)
{
    struct timespec clock_value;
    clock_gettime(CLOCK_MONOTONIC, &clock_value);

    return (double)clock_value.tv_sec * MS_PER_SECOND +
           (double)clock_value.tv_nsec / NS_PER_MS;
}

/* 지금 기록할 요청을 비운 상태로 시작합니다. */
static void stage_record_start(const char *kind, unsigned long connection_id,
                               double started_ms)
{
    if (stage_trace_state.batch_started_ms == 0) {
        stage_trace_state.batch_started_ms = started_ms;
    }
    struct stage_record *record = &stage_trace_state.current;
    record->kind = kind;
    record->connection_id = connection_id;
    record->batch_number = stage_trace_state.batch_number;
    record->started_ms = started_ms;
    record->sent_ms = 0;
    record->is_bundled = false;
    record->mget_keys = 0;
    record->mget_number = 0;
    record->is_store_deferred = false;
    for (int i = 0; i < STAGE_COUNT; i++) {
        record->stage_ms[i] = STAGE_NOT_ENTERED;
    }
}

/* request_handle과 같은 순서로 경로를 나눕니다. stats는 cache_bench.py가 측정
   구간의 앞뒤에서 부르므로, 요약 스크립트가 측정 구간을 고르는 표시가 됩니다. */
static const char *stage_kind_find(const struct mg_http_message *message)
{
    if (mg_match(message->uri, mg_str("/stats"), NULL)) {
        return "stats";
    }
    if (mg_match(message->uri, mg_str("/products"), NULL)) {
        return "list";
    }
    if (path_is_product(message->uri)) {
        return method_is(message, "GET") ? "detail" : "change";
    }

    return "other";
}

static void stage_batch_sent(struct stage_batch *batch,
                             unsigned long connection_id, double now)
{
    for (size_t i = 0; i < batch->request_count && i < STAGE_BATCH_MAX; i++) {
        struct stage_record *record = &batch->records[i];
        if (record->connection_id == connection_id && record->sent_ms == 0) {
            record->sent_ms = now;
        }
    }
}

static void stage_batch_write(const struct stage_batch *batch)
{
    for (size_t i = 0; i < batch->request_count && i < STAGE_BATCH_MAX; i++) {
        stage_record_write(&batch->records[i]);
    }
    for (size_t i = 0; i < batch->store_count; i++) {
        const struct stage_store *store = &batch->stores[i];
        fprintf(stage_trace_state.file, "store %llu %s %zu %.3f\n",
                store->batch_number, store->key_count >= 2 ? "mset" : "put",
                store->key_count, store->stored_ms);
    }
}

static void stage_record_write(const struct stage_record *record)
{
    FILE *file = stage_trace_state.file;
    fprintf(file, "stage %llu %s %.3f %.3f", record->batch_number, record->kind,
            record->waited_ms, record->finished_ms - record->started_ms);
    for (int i = 0; i < STAGE_COUNT; i++) {
        if (record->stage_ms[i] < 0) {
            fputs(" -", file);
        } else {
            fprintf(file, " %.3f", record->stage_ms[i]);
        }
    }
    fprintf(file, " %.3f", record->held_ms);
    if (record->sent_ms == 0) {
        fputs(" -", file);
    } else {
        fprintf(file, " %.3f", record->sent_ms - record->finished_ms);
    }
    if (record->is_bundled) {
        fprintf(file, " 1 %zu %llu", record->mget_keys, record->mget_number);
    } else {
        fputs(" 0 - -", file);
    }
    fprintf(file, " %zu\n", record->batch_size);
}
#endif
