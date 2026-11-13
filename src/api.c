#include "api.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define KEYS_PREFIX "/keys/"
#define KEYS_PREFIX_LEN (sizeof KEYS_PREFIX - 1)
/* 오류 응답의 body입니다. 리터럴을 이어 붙여 컴파일 때 문자열 하나가 됩니다. */
#define ERROR_BODY(reason) "{\"error\":\"" reason "\"}"

/* 지원하는 메서드가 store에 하는 일입니다. */
enum key_action {
    KEY_ACTION_NONE,
    KEY_ACTION_GET,
    KEY_ACTION_SET,
    KEY_ACTION_DELETE,
};

static bool path_is_keys(const char *path, size_t path_len);
static enum key_action key_action_parse(const char *method, size_t method_len);
static bool method_is(const char *method, size_t method_len, const char *name);
static struct api_response
key_request_handle(struct store *store, enum key_action action,
                   const char *encoded_key, size_t encoded_key_len,
                   const void *body, size_t body_len);
static bool key_decode(const char *encoded, size_t encoded_len, char *key,
                       size_t *key_len);
static int hex_digit_value(char digit);
static struct api_response key_action_run(struct store *store,
                                          enum key_action action,
                                          const char *key, size_t key_len,
                                          const void *body, size_t body_len);
static struct api_response key_get(struct store *store, const char *key,
                                   size_t key_len);
static struct api_response key_set(struct store *store, const char *key,
                                   size_t key_len, const void *value,
                                   size_t value_len);
static struct api_response key_delete(struct store *store, const char *key,
                                      size_t key_len);
static struct api_response response_error(int status, const char *body);

/* 경로, 메서드, key 순서로 검사하고 처음 걸린 오류 하나로 답합니다. 경로가
   틀린 요청에 메서드나 key 오류를 알리면 다른 서버가 엉뚱한 곳을 고치게
   되기 때문입니다(spec "오류의 우선순위"). */
struct api_response api_handle(struct store *store, const char *method,
                               size_t method_len, const char *path,
                               size_t path_len, const void *body,
                               size_t body_len)
{
    if (!path_is_keys(path, path_len)) {
        return response_error(404, ERROR_BODY("route_not_found"));
    }
    enum key_action action = key_action_parse(method, method_len);
    if (action == KEY_ACTION_NONE) {
        return response_error(405, ERROR_BODY("method_not_allowed"));
    }

    return key_request_handle(store, action, path + KEYS_PREFIX_LEN,
                              path_len - KEYS_PREFIX_LEN, body, body_len);
}

bool api_reject_large_body(size_t body_len, struct api_response *response)
{
    if (body_len <= API_BODY_MAX) {
        return false;
    }
    *response = response_error(413, ERROR_BODY("body_too_large"));

    return true;
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
static struct api_response key_request_handle(struct store *store,
                                              enum key_action action,
                                              const char *encoded_key,
                                              size_t encoded_key_len,
                                              const void *body, size_t body_len)
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
        response = key_action_run(store, action, key, key_len, body, body_len);
    }
    /* store는 key를 복사해서 가지므로, 응답을 돌려주기 전에 풀어도 됩니다. */
    free(key);

    return response;
}

/* %HH를 정확히 한 번 풀어 key에 쓰고, 그 길이를 *key_len에 둡니다. 푼 결과는
   다시 보지 않으므로 %2541은 %41이 됩니다. %00도 길이에 들어가 잘리지 않습니다.
   %가 16진수 두 자리로 이어지지 않으면 false를 돌려줍니다. */
static bool key_decode(const char *encoded, size_t encoded_len, char *key,
                       size_t *key_len)
{
    size_t written = 0;
    for (size_t i = 0; i < encoded_len; i++) {
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
static struct api_response key_action_run(struct store *store,
                                          enum key_action action,
                                          const char *key, size_t key_len,
                                          const void *body, size_t body_len)
{
    if (action == KEY_ACTION_GET) {
        return key_get(store, key, key_len);
    }
    if (action == KEY_ACTION_SET) {
        return key_set(store, key, key_len, body, body_len);
    }

    return key_delete(store, key, key_len);
}

static struct api_response key_get(struct store *store, const char *key,
                                   size_t key_len)
{
    const void *value = NULL;
    size_t value_len = 0;
    if (!store_get(store, key, key_len, &value, &value_len)) {
        return response_error(404, ERROR_BODY("key_not_found"));
    }
    struct api_response response = {200, "application/octet-stream", value,
                                    value_len};

    return response;
}

static struct api_response key_set(struct store *store, const char *key,
                                   size_t key_len, const void *value,
                                   size_t value_len)
{
    /* store_set의 false는 메모리 부족뿐입니다. 할당 함수를 바꿔 끼울 seam이
       없어서 이 분기는 자동 테스트하지 않고 리뷰로 확인합니다. */
    if (!store_set(store, key, key_len, value, value_len)) {
        return response_error(500, ERROR_BODY("out_of_memory"));
    }
    struct api_response response = {204, NULL, NULL, 0};

    return response;
}

static struct api_response key_delete(struct store *store, const char *key,
                                      size_t key_len)
{
    if (!store_delete(store, key, key_len)) {
        return response_error(404, ERROR_BODY("key_not_found"));
    }
    struct api_response response = {204, NULL, NULL, 0};

    return response;
}

/* body는 오류 이유를 담은 JSON 리터럴(ERROR_BODY)입니다. */
static struct api_response response_error(int status, const char *body)
{
    struct api_response response = {status, "application/json", body,
                                    strlen(body)};
    return response;
}
