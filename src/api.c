#include "api.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KEYS_PREFIX "/keys/"
#define KEYS_PREFIX_LEN (sizeof KEYS_PREFIX - 1)
/* 오류 응답의 body입니다. 리터럴을 이어 붙여 컴파일 때 문자열 하나가 됩니다. */
#define ERROR_BODY(reason) "{\"error\":\"" reason "\"}"
/* 405 응답의 Allow 헤더 값입니다. key_action_parse가 받는 메서드와 같습니다. */
#define ALLOWED_METHODS "GET, PUT, DELETE"
#define MILLIS_PER_SECOND 1000
#define MGET_PATH "/mget"
#define MGET_PATH_LEN (sizeof MGET_PATH - 1)
#define MGET_ALLOWED_METHODS "POST"
/* POST /mget 응답에서 없는 key 하나의 자리입니다. */
#define MGET_MISSING "-\n"
/* "<value 바이트 길이>\n"의 최대 길이입니다. size_t 20자리와 \n, '\0'이
   들어갑니다. */
#define MGET_LENGTH_TEXT_MAX 24
#define MSET_PATH "/mset"
#define MSET_PATH_LEN (sizeof MSET_PATH - 1)
#define MSET_ALLOWED_METHODS "POST"
#define DECIMAL_BASE 10
#define BUFFER_CAPACITY_INITIAL 256

/* 지원하는 메서드가 store에 하는 일입니다. */
enum key_action {
    KEY_ACTION_NONE,
    KEY_ACTION_GET,
    KEY_ACTION_SET,
    KEY_ACTION_DELETE,
};

/* POST /mget body에서 푼 key 하나가 푼 key 버퍼에서 차지하는 자리입니다. */
struct mget_key {
    size_t start;
    size_t len;
};

/* POST /mset body의 항목 하나입니다. key는 인코딩된 채로 body 안을 가리키고,
   푼 key는 푼 key 버퍼의 key_start부터 key_len바이트입니다. value는 body 안을
   가리킵니다. */
struct mset_item {
    const char *encoded_key;
    size_t encoded_key_len;
    size_t key_start;
    size_t key_len;
    const char *value;
    size_t value_len;
};

/* 응답 body를 이어 붙이며 늘어나는 버퍼입니다. */
struct byte_buffer {
    char *data;
    size_t len;
    size_t capacity;
};

static bool path_is_mget(const char *path, size_t path_len);
static struct api_response
mget_request_handle(struct store *store, uint64_t now, const char *method,
                    size_t method_len, const char *body, size_t body_len);
static size_t mget_line_count(const char *body, size_t body_len);
static const char *mget_keys_parse(const char *body, size_t body_len,
                                   char *decoded, struct mget_key *keys);
static struct api_response
mget_values_collect(struct store *store, uint64_t now, const char *decoded,
                    const struct mget_key *keys, size_t key_count);
static bool mget_value_append(struct byte_buffer *buffer, const void *value,
                              size_t value_len);
static bool byte_buffer_append(struct byte_buffer *buffer, const void *bytes,
                               size_t bytes_len);
static bool path_is_mset(const char *path, size_t path_len);
static struct api_response
mset_request_handle(struct store *store, const char *method, size_t method_len,
                    const struct api_ttl_header *ttl_header, const char *body,
                    size_t body_len);
static bool mset_items_split(const char *body, size_t body_len,
                             struct mset_item *items, size_t *item_count);
static bool mset_value_len_parse(const char *text, size_t text_len,
                                 size_t *value_len);
static const char *mset_keys_decode(struct mset_item *items, size_t item_count,
                                    char *decoded);
static struct api_response mset_items_store(struct store *store,
                                            const char *decoded,
                                            const struct mset_item *items,
                                            size_t item_count,
                                            uint64_t expires_at);
static bool path_is_keys(const char *path, size_t path_len);
static enum key_action key_action_parse(const char *method, size_t method_len);
static bool method_is(const char *method, size_t method_len, const char *name);
static struct api_response
key_request_handle(struct store *store, uint64_t now, enum key_action action,
                   const char *encoded_key, size_t encoded_key_len,
                   const struct api_ttl_header *ttl_header, const void *body,
                   size_t body_len);
static bool key_decode(const char *encoded, size_t encoded_len, char *key,
                       size_t *key_len);
static int hex_digit_value(char digit);
static struct api_response
key_action_run(struct store *store, uint64_t now, enum key_action action,
               const char *key, size_t key_len,
               const struct api_ttl_header *ttl_header, const void *body,
               size_t body_len);
static struct api_response key_get(struct store *store, uint64_t now,
                                   const char *key, size_t key_len);
static struct api_response key_set(struct store *store, const char *key,
                                   size_t key_len,
                                   const struct api_ttl_header *ttl_header,
                                   const void *value, size_t value_len);
static bool ttl_header_parse(const struct api_ttl_header *ttl_header,
                             uint64_t *expires_at);
static bool ttl_seconds_parse(const char *text, size_t text_len,
                              uint64_t *seconds);
static struct api_response key_delete(struct store *store, uint64_t now,
                                      const char *key, size_t key_len);
static struct api_response response_error(int status, const char *body);
static struct api_response response_no_content(void);

/* 경로, 메서드, key 순서로 검사하고 처음 걸린 오류 하나로 답합니다. 경로가
   틀린 요청에 메서드나 key 오류를 알리면 client가 엉뚱한 곳을 고치게
   되기 때문입니다(spec "오류의 우선순위"). */
struct api_response api_handle(struct store *store, uint64_t now,
                               const char *method, size_t method_len,
                               const char *path, size_t path_len,
                               const struct api_ttl_header *ttl_header,
                               const void *body, size_t body_len)
{
    assert(ttl_header != NULL);
    if (path_is_mget(path, path_len)) {
        return mget_request_handle(store, now, method, method_len, body,
                                   body_len);
    }
    if (path_is_mset(path, path_len)) {
        return mset_request_handle(store, method, method_len, ttl_header, body,
                                   body_len);
    }
    if (!path_is_keys(path, path_len)) {
        return response_error(404, ERROR_BODY("route_not_found"));
    }
    enum key_action action = key_action_parse(method, method_len);
    if (action == KEY_ACTION_NONE) {
        struct api_response response =
            response_error(405, ERROR_BODY("method_not_allowed"));
        response.allow = ALLOWED_METHODS;
        return response;
    }

    return key_request_handle(store, now, action, path + KEYS_PREFIX_LEN,
                              path_len - KEYS_PREFIX_LEN, ttl_header, body,
                              body_len);
}

/* 서버가 상한 너머의 헤더를 읽지 못하면 그 안의 TTL, Transfer-Encoding,
   Content-Length를 모른 채 처리하게 되므로, 헤더가 넘친 요청을 가장 먼저
   거절합니다. chunked 요청은 body 길이를 끝까지 받아야 알 수 있어서, 상한을
   body를 받기 전에 검사할 수 없습니다. 그래서 Content-Length로 길이를 알리게
   합니다. */
bool api_reject_headers(bool has_too_many_headers, bool has_transfer_encoding,
                        size_t body_len, struct api_response *response)
{
    if (has_too_many_headers) {
        *response = response_error(431, ERROR_BODY("too_many_headers"));
        return true;
    }
    if (has_transfer_encoding) {
        *response = response_error(411, ERROR_BODY("length_required"));
        return true;
    }
    if (body_len > API_BODY_MAX) {
        *response = response_error(413, ERROR_BODY("body_too_large"));
        return true;
    }

    return false;
}

struct api_response api_reject_timeout(void)
{
    return response_error(408, ERROR_BODY("request_timeout"));
}

struct api_response api_reject_connection(void)
{
    return response_error(503, ERROR_BODY("too_many_connections"));
}

void api_response_destroy(struct api_response *response)
{
    if (response->owned_body == NULL) {
        return;
    }
    free(response->owned_body);
    response->owned_body = NULL;
    response->body = NULL;
    response->body_len = 0;
}

/* query가 붙은 /mget?x나 /mget/는 이 경로가 아닙니다. */
static bool path_is_mget(const char *path, size_t path_len)
{
    return path_len == MGET_PATH_LEN && memcmp(path, MGET_PATH, path_len) == 0;
}

/* 메서드, key 개수, key마다의 인코딩, value 합계 순서로 검사하고 처음 걸린
   오류 하나로 답합니다. 요청의 모양이 틀린 오류를 저장된 데이터에 따라 달라지는
   response_too_large보다 먼저 알립니다. */
static struct api_response
mget_request_handle(struct store *store, uint64_t now, const char *method,
                    size_t method_len, const char *body, size_t body_len)
{
    if (!method_is(method, method_len, "POST")) {
        struct api_response response =
            response_error(405, ERROR_BODY("method_not_allowed"));
        response.allow = MGET_ALLOWED_METHODS;
        return response;
    }
    size_t key_count = mget_line_count(body, body_len);
    if (key_count == 0) {
        return response_error(400, ERROR_BODY("bad_request"));
    }
    if (key_count > API_MGET_KEYS_MAX) {
        return response_error(400, ERROR_BODY("too_many_keys"));
    }
    /* 푼 key는 인코딩된 줄보다 길지 않으므로 body 길이면 모든 key가 담깁니다.
       key가 하나 이상이면 body는 1바이트 이상입니다. */
    char *decoded = malloc(body_len);
    if (decoded == NULL) {
        return response_error(500, ERROR_BODY("out_of_memory"));
    }
    struct mget_key keys[API_MGET_KEYS_MAX];
    const char *error_body = mget_keys_parse(body, body_len, decoded, keys);
    struct api_response response =
        error_body != NULL
            ? response_error(400, error_body)
            : mget_values_collect(store, now, decoded, keys, key_count);
    free(decoded);

    return response;
}

/* body의 key 수입니다. 줄은 \n으로 끝나고 마지막 줄만 \n이 없어도 됩니다.
   그래서 "a\n"은 1개, "\n"은 빈 줄 1개, 빈 body는 0개입니다. */
static size_t mget_line_count(const char *body, size_t body_len)
{
    if (body_len == 0) {
        return 0;
    }
    size_t line_count = body[body_len - 1] == '\n' ? 0 : 1;
    for (size_t i = 0; i < body_len; i++) {
        if (body[i] == '\n') {
            line_count++;
        }
    }

    return line_count;
}

/* 줄마다 /keys/<key>와 같은 규칙으로 key를 풀어 decoded에 이어 쓰고, 그 자리를
   keys에 적습니다. 앞 줄부터 보고 처음 걸린 오류의 body를 돌려주며, 모두 맞으면
   NULL을 돌려줍니다. keys는 mget_line_count가 센 개수만큼 자리가 있어야
   합니다. */
static const char *mget_keys_parse(const char *body, size_t body_len,
                                   char *decoded, struct mget_key *keys)
{
    size_t line_start = 0;
    size_t written = 0;
    for (size_t i = 0; line_start < body_len; i++) {
        const char *line = body + line_start;
        const char *line_end = memchr(line, '\n', body_len - line_start);
        size_t line_len = line_end == NULL ? body_len - line_start
                                           : (size_t)(line_end - line);
        size_t key_len = 0;
        if (!key_decode(line, line_len, decoded + written, &key_len)) {
            return ERROR_BODY("bad_key_encoding");
        }
        if (key_len == 0) {
            return ERROR_BODY("empty_key");
        }
        keys[i].start = written;
        keys[i].len = key_len;
        written += key_len;
        line_start += line_len + 1;
    }

    return NULL;
}

/* key 순서대로 "<바이트 길이>\n<value>\n"을, 없거나 만료된 key는 "-\n"을 이어
   붙입니다. 같은 key가 여러 번 오면 매번 붙입니다. value 합계가
   API_MGET_VALUES_MAX를 넘으면 만든 버퍼를 버리고 400을 돌려줍니다. */
static struct api_response
mget_values_collect(struct store *store, uint64_t now, const char *decoded,
                    const struct mget_key *keys, size_t key_count)
{
    struct byte_buffer buffer = {0};
    size_t values_len = 0;
    bool is_appended = true;
    for (size_t i = 0; i < key_count && is_appended; i++) {
        const void *value = NULL;
        size_t value_len = 0;
        if (!store_get(store, now, decoded + keys[i].start, keys[i].len, &value,
                       &value_len)) {
            is_appended =
                byte_buffer_append(&buffer, MGET_MISSING, strlen(MGET_MISSING));
            continue;
        }
        values_len += value_len;
        if (values_len > API_MGET_VALUES_MAX) {
            free(buffer.data);
            return response_error(400, ERROR_BODY("response_too_large"));
        }
        /* 빌린 value는 다음 store 호출 전에 응답 버퍼로 복사합니다(ADR
           get-lends-value-until-next-change). */
        is_appended = mget_value_append(&buffer, value, value_len);
    }
    if (!is_appended) {
        free(buffer.data);
        return response_error(500, ERROR_BODY("out_of_memory"));
    }
    struct api_response response = {.status = 200,
                                    .content_type = "application/octet-stream",
                                    .body = buffer.data,
                                    .body_len = buffer.len,
                                    .owned_body = buffer.data};

    return response;
}

static bool mget_value_append(struct byte_buffer *buffer, const void *value,
                              size_t value_len)
{
    char length_text[MGET_LENGTH_TEXT_MAX];
    int length_text_len =
        snprintf(length_text, sizeof length_text, "%zu\n", value_len);

    return byte_buffer_append(buffer, length_text, (size_t)length_text_len) &&
           byte_buffer_append(buffer, value, value_len) &&
           byte_buffer_append(buffer, "\n", 1);
}

/* 모자라면 용량을 두 배씩 늘립니다. 메모리가 없으면 false를 돌려주고, 이미 쓴
   내용은 그대로 둡니다. */
static bool byte_buffer_append(struct byte_buffer *buffer, const void *bytes,
                               size_t bytes_len)
{
    if (bytes_len == 0) {
        return true;
    }
    if (buffer->len + bytes_len > buffer->capacity) {
        size_t capacity =
            buffer->capacity == 0 ? BUFFER_CAPACITY_INITIAL : buffer->capacity;
        while (capacity < buffer->len + bytes_len) {
            capacity *= 2;
        }
        char *data = realloc(buffer->data, capacity);
        if (data == NULL) {
            return false;
        }
        buffer->data = data;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->len, bytes, bytes_len);
    buffer->len += bytes_len;

    return true;
}

/* query가 붙은 /mset?x나 /mset/는 이 경로가 아닙니다. */
static bool path_is_mset(const char *path, size_t path_len)
{
    return path_len == MSET_PATH_LEN && memcmp(path, MSET_PATH, path_len) == 0;
}

/* 메서드, 항목의 모양, key 개수, key마다의 인코딩, TTL 순서로 요청 전체를 먼저
   검사하고, 처음 걸린 오류 하나로 답합니다. 검사에 걸린 요청은 아무 key도
   저장하지 않습니다. 뒤 항목이 틀렸는데 앞 key가 저장되면 client가 무엇이
   남았는지 알 수 없기 때문입니다. 검사를 통과하면 mset_items_store가 저장합니다. */
static struct api_response
mset_request_handle(struct store *store, const char *method, size_t method_len,
                    const struct api_ttl_header *ttl_header, const char *body,
                    size_t body_len)
{
    if (!method_is(method, method_len, "POST")) {
        struct api_response response =
            response_error(405, ERROR_BODY("method_not_allowed"));
        response.allow = MSET_ALLOWED_METHODS;
        return response;
    }
    struct mset_item items[API_MSET_KEYS_MAX];
    size_t item_count = 0;
    if (!mset_items_split(body, body_len, items, &item_count) ||
        item_count == 0) {
        return response_error(400, ERROR_BODY("bad_request"));
    }
    if (item_count > API_MSET_KEYS_MAX) {
        return response_error(400, ERROR_BODY("too_many_keys"));
    }
    /* 푼 key는 인코딩된 key보다 길지 않으므로 body 길이면 모든 key가 담깁니다.
       항목이 하나 이상이면 body는 1바이트 이상입니다. */
    char *decoded = malloc(body_len);
    if (decoded == NULL) {
        return response_error(500, ERROR_BODY("out_of_memory"));
    }
    uint64_t expires_at = STORE_NO_EXPIRY;
    const char *error_body = mset_keys_decode(items, item_count, decoded);
    struct api_response response;
    if (error_body != NULL) {
        response = response_error(400, error_body);
    } else if (!ttl_header_parse(ttl_header, &expires_at)) {
        response = response_error(400, ERROR_BODY("bad_ttl"));
    } else {
        response =
            mset_items_store(store, decoded, items, item_count, expires_at);
    }
    free(decoded);

    return response;
}

/* body를 "<key>\n<value 바이트 길이>\n<value>\n" 항목들로 나눕니다. 모양이 틀린
   항목이 하나라도 있으면 false를 돌려줍니다. 항목 수는 상한을 넘어도 끝까지
   세어 *item_count에 쓰고, items에는 앞의 API_MSET_KEYS_MAX개만 적습니다. */
static bool mset_items_split(const char *body, size_t body_len,
                             struct mset_item *items, size_t *item_count)
{
    size_t position = 0;
    size_t count = 0;
    while (position < body_len) {
        const char *key = body + position;
        const char *key_end = memchr(key, '\n', body_len - position);
        if (key_end == NULL) {
            return false;
        }
        const char *length_text = key_end + 1;
        size_t length_room = body_len - (size_t)(length_text - body);
        const char *length_end = memchr(length_text, '\n', length_room);
        size_t value_len = 0;
        if (length_end == NULL ||
            !mset_value_len_parse(
                length_text, (size_t)(length_end - length_text), &value_len)) {
            return false;
        }
        size_t value_start = (size_t)(length_end - body) + 1;
        /* value 뒤에 \n 한 바이트가 더 있어야 합니다. */
        if (value_len >= body_len - value_start ||
            body[value_start + value_len] != '\n') {
            return false;
        }
        if (count < API_MSET_KEYS_MAX) {
            items[count] =
                (struct mset_item){.encoded_key = key,
                                   .encoded_key_len = (size_t)(key_end - key),
                                   .value = body + value_start,
                                   .value_len = value_len};
        }
        count++;
        position = value_start + value_len + 1;
    }
    *item_count = count;

    return true;
}

/* value 바이트 길이는 10진수 숫자로만 된 0 이상의 정수입니다. 부호, 공백, 빈
   값은 받지 않습니다. body 상한보다 큰 길이는 body에 담길 수 없으므로 거절해
   넘침을 막습니다. */
static bool mset_value_len_parse(const char *text, size_t text_len,
                                 size_t *value_len)
{
    if (text_len == 0) {
        return false;
    }
    size_t parsed = 0;
    for (size_t i = 0; i < text_len; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        parsed = parsed * DECIMAL_BASE + (size_t)(text[i] - '0');
        if (parsed > API_BODY_MAX) {
            return false;
        }
    }
    *value_len = parsed;

    return true;
}

/* 항목마다 /keys/<key>와 같은 규칙으로 key를 풀어 decoded에 이어 쓰고, 그 자리를
   항목에 적습니다. 앞 항목부터 보고 처음 걸린 오류의 body를 돌려주며, 모두
   맞으면 NULL을 돌려줍니다. */
static const char *mset_keys_decode(struct mset_item *items, size_t item_count,
                                    char *decoded)
{
    size_t written = 0;
    for (size_t i = 0; i < item_count; i++) {
        size_t key_len = 0;
        if (!key_decode(items[i].encoded_key, items[i].encoded_key_len,
                        decoded + written, &key_len)) {
            return ERROR_BODY("bad_key_encoding");
        }
        if (key_len == 0) {
            return ERROR_BODY("empty_key");
        }
        items[i].key_start = written;
        items[i].key_len = key_len;
        written += key_len;
    }

    return NULL;
}

/* 입력 순서대로 저장합니다. 같은 key가 여러 번 오면 뒤의 value가 남습니다.
   store_set은 메모리 부족에만 실패하며, 그러면 앞 항목은 저장된 채로 두고 그
   항목부터는 저장하지 않은 채 500을 돌려줍니다. 되돌리려면 덮어쓴 옛 value를
   모두 들고 있어야 하므로, 이 API는 전부 성공하거나 전부 실패하는 연산이 아닙니다
   (docs/api.md "여러 key 저장"). 할당 함수를 바꿔 끼울 seam이 없어서 이 분기는
   자동 테스트하지 않고 리뷰로 확인합니다. */
static struct api_response mset_items_store(struct store *store,
                                            const char *decoded,
                                            const struct mset_item *items,
                                            size_t item_count,
                                            uint64_t expires_at)
{
    for (size_t i = 0; i < item_count; i++) {
        if (!store_set(store, decoded + items[i].key_start, items[i].key_len,
                       items[i].value, items[i].value_len, expires_at)) {
            return response_error(500, ERROR_BODY("out_of_memory"));
        }
    }

    return response_no_content();
}

/* `/keys/`로 시작해야 합니다. 끝에 /가 없는 `/keys`도 경로 오류입니다. */
static bool path_is_keys(const char *path, size_t path_len)
{
    return path_len >= KEYS_PREFIX_LEN &&
           memcmp(path, KEYS_PREFIX, KEYS_PREFIX_LEN) == 0;
}

static enum key_action key_action_parse(const char *method, size_t method_len)
{
    if (method_is(method, method_len, "GET")) {
        return KEY_ACTION_GET;
    }
    if (method_is(method, method_len, "PUT")) {
        return KEY_ACTION_SET;
    }
    if (method_is(method, method_len, "DELETE")) {
        return KEY_ACTION_DELETE;
    }

    return KEY_ACTION_NONE;
}

static bool method_is(const char *method, size_t method_len, const char *name)
{
    return method_len == strlen(name) && memcmp(method, name, method_len) == 0;
}

/* key의 인코딩을 풀어 검사한 뒤 store에 action을 합니다. */
static struct api_response
key_request_handle(struct store *store, uint64_t now, enum key_action action,
                   const char *encoded_key, size_t encoded_key_len,
                   const struct api_ttl_header *ttl_header, const void *body,
                   size_t body_len)
{
    /* 푼 key는 인코딩된 key보다 길지 않습니다. 0바이트 malloc을 피하려고 1을 더합니다. */
    char *key = malloc(encoded_key_len + 1);
    if (key == NULL) {
        return response_error(500, ERROR_BODY("out_of_memory"));
    }
    size_t key_len = 0;
    struct api_response response;
    if (!key_decode(encoded_key, encoded_key_len, key, &key_len)) {
        response = response_error(400, ERROR_BODY("bad_key_encoding"));
    } else if (key_len == 0) {
        /* store는 0바이트 key를 받지만, HTTP 입구에서는 "key 목록"으로 읽힐
           여지를 없애려고 거절합니다(spec Q3). */
        response = response_error(400, ERROR_BODY("empty_key"));
    } else {
        response = key_action_run(store, now, action, key, key_len, ttl_header,
                                  body, body_len);
    }
    /* store는 key를 복사해서 가지므로, 응답을 돌려주기 전에 풀어도 됩니다. */
    free(key);

    return response;
}

/* %HH를 정확히 한 번 풀어 key에 쓰고, 그 길이를 *key_len에 둡니다. 푼 결과는
   다시 보지 않으므로 %2541은 %41이 됩니다. %00도 길이에 들어가 잘리지 않습니다.
   %가 16진수 두 자리로 이어지지 않거나 인코딩하지 않은 ?가 있으면 false를
   돌려줍니다. */
static bool key_decode(const char *encoded, size_t encoded_len, char *key,
                       size_t *key_len)
{
    size_t written = 0;
    for (size_t i = 0; i < encoded_len; i++) {
        /* ?는 HTTP에서 경로와 query를 나누는 문자라서, 인코딩하지 않은 ?를
           key의 바이트로 쓰면 client와 라이브러리가 key를 다르게 읽습니다.
           key 안의 ?는 %3F로 보내야 합니다(spec Q3). */
        if (encoded[i] == '?') {
            return false;
        }
        if (encoded[i] != '%') {
            key[written++] = encoded[i];
            continue;
        }
        if (encoded_len - i < 3) {
            return false;
        }
        int high = hex_digit_value(encoded[i + 1]);
        int low = hex_digit_value(encoded[i + 2]);
        if (high < 0 || low < 0) {
            return false;
        }
        key[written++] = (char)(high * 16 + low);
        i += 2;
    }
    *key_len = written;

    return true;
}

/* 16진수 한 자리의 값을 돌려주고, 16진수가 아니면 -1을 돌려줍니다. */
static int hex_digit_value(char digit)
{
    if (digit >= '0' && digit <= '9') {
        return digit - '0';
    }
    if (digit >= 'a' && digit <= 'f') {
        return digit - 'a' + 10;
    }
    if (digit >= 'A' && digit <= 'F') {
        return digit - 'A' + 10;
    }

    return -1;
}

/* action은 key_action_parse가 KEY_ACTION_NONE이 아니라고 확인한 값입니다. */
static struct api_response
key_action_run(struct store *store, uint64_t now, enum key_action action,
               const char *key, size_t key_len,
               const struct api_ttl_header *ttl_header, const void *body,
               size_t body_len)
{
    if (action == KEY_ACTION_GET) {
        return key_get(store, now, key, key_len);
    }
    if (action == KEY_ACTION_SET) {
        return key_set(store, key, key_len, ttl_header, body, body_len);
    }

    return key_delete(store, now, key, key_len);
}

static struct api_response key_get(struct store *store, uint64_t now,
                                   const char *key, size_t key_len)
{
    const void *value = NULL;
    size_t value_len = 0;
    if (!store_get(store, now, key, key_len, &value, &value_len)) {
        return response_error(404, ERROR_BODY("key_not_found"));
    }
    struct api_response response = {.status = 200,
                                    .content_type = "application/octet-stream",
                                    .body = value,
                                    .body_len = value_len};

    return response;
}

/* PUT은 entry를 통째로 바꾸므로 보관 기간도 이 요청이 정하고, TTL 헤더가
   없으면 기간이 사라집니다(spec Q7). */
static struct api_response key_set(struct store *store, const char *key,
                                   size_t key_len,
                                   const struct api_ttl_header *ttl_header,
                                   const void *value, size_t value_len)
{
    uint64_t expires_at = STORE_NO_EXPIRY;
    /* 잘못된 TTL을 "만료 없음"으로 저장하면 client의 실수가 조용히 사라지므로,
       store를 부르기 전에 거절합니다(spec Q14). */
    if (!ttl_header_parse(ttl_header, &expires_at)) {
        return response_error(400, ERROR_BODY("bad_ttl"));
    }
    /* store_set의 false는 메모리 부족뿐입니다. 할당 함수를 바꿔 끼울 seam이
       없어서 이 분기는 자동 테스트하지 않고 리뷰로 확인합니다. */
    if (!store_set(store, key, key_len, value, value_len, expires_at)) {
        return response_error(500, ERROR_BODY("out_of_memory"));
    }

    return response_no_content();
}

/* TTL 헤더를 만료 시각으로 바꿔 *expires_at에 쓰고 true를 돌려줍니다. 헤더가
   없으면 STORE_NO_EXPIRY입니다. 헤더가 두 번 이상 왔거나, 값이 1 이상의 정수
   초가 아니거나, ms로 바꿔 받은 시각에 더하면 STORE_NO_EXPIRY에 닿으면 false를
   돌려줍니다. */
static bool ttl_header_parse(const struct api_ttl_header *ttl_header,
                             uint64_t *expires_at)
{
    if (ttl_header->count == 0) {
        *expires_at = STORE_NO_EXPIRY;
        return true;
    }
    /* 어느 값이 쓰였는지 client가 모르는 채 204를 받지 않게 합니다(spec Q15). */
    if (ttl_header->count > 1) {
        return false;
    }
    uint64_t seconds = 0;
    /* 만료 시각이 STORE_NO_EXPIRY에 닿지 않게 받은 시각에 더할 수 있는 ms입니다. */
    uint64_t millis_left = STORE_NO_EXPIRY - 1 - ttl_header->received_at;
    if (!ttl_seconds_parse(ttl_header->text, ttl_header->text_len, &seconds) ||
        seconds > millis_left / MILLIS_PER_SECOND) {
        return false;
    }
    *expires_at = ttl_header->received_at + seconds * MILLIS_PER_SECOND;

    return true;
}

/* 10진수 숫자로만 된 1 이상의 정수를 *seconds에 씁니다. 부호, 공백, 빈 값,
   uint64_t를 넘는 값은 false입니다. */
static bool ttl_seconds_parse(const char *text, size_t text_len,
                              uint64_t *seconds)
{
    uint64_t parsed = 0;
    for (size_t i = 0; i < text_len; i++) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        uint64_t digit = (uint64_t)(text[i] - '0');
        if (parsed > (UINT64_MAX - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }
    *seconds = parsed;

    return parsed >= 1;
}

static struct api_response key_delete(struct store *store, uint64_t now,
                                      const char *key, size_t key_len)
{
    if (!store_delete(store, now, key, key_len)) {
        return response_error(404, ERROR_BODY("key_not_found"));
    }

    return response_no_content();
}

/* body는 오류 이유를 담은 JSON 리터럴(ERROR_BODY)입니다. */
static struct api_response response_error(int status, const char *body)
{
    struct api_response response = {.status = status,
                                    .content_type = "application/json",
                                    .body = body,
                                    .body_len = strlen(body)};

    return response;
}

/* 저장과 삭제의 성공입니다. 204에는 body도 Content-Type도 없습니다. */
static struct api_response response_no_content(void)
{
    struct api_response response = {.status = 204};
    return response;
}
