#include "api.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define KEYS_PREFIX "/keys/"
#define KEYS_PREFIX_LEN (sizeof KEYS_PREFIX - 1)
/* 오류 응답의 body입니다. 리터럴을 이어 붙여 컴파일 때 문자열 하나가 됩니다. */
#define ERROR_BODY(reason) "{\"error\":\"" reason "\"}"

static struct api_response key_dispatch(struct store *store, const char *method,
                                        size_t method_len, const char *key,
                                        size_t key_len, const void *body,
                                        size_t body_len);
static bool key_decode(const char *encoded, size_t encoded_len, char *key,
                       size_t *key_len);
static int hex_digit_value(char digit);
static bool method_is(const char *method, size_t method_len, const char *name);
static struct api_response key_get(struct store *store, const char *key,
                                   size_t key_len);
static struct api_response key_set(struct store *store, const char *key,
                                   size_t key_len, const void *value,
                                   size_t value_len);
static struct api_response key_delete(struct store *store, const char *key,
                                      size_t key_len);
static struct api_response response_error(int status, const char *body);
static struct api_response response_unhandled(void);

struct api_response api_handle(struct store *store, const char *method,
                               size_t method_len, const char *path,
                               size_t path_len, const void *body,
                               size_t body_len)
{
    if (path_len < KEYS_PREFIX_LEN ||
        memcmp(path, KEYS_PREFIX, KEYS_PREFIX_LEN) != 0) {
        return response_unhandled();
    }
    const char *encoded_key = path + KEYS_PREFIX_LEN;
    size_t encoded_key_len = path_len - KEYS_PREFIX_LEN;
    /* 푼 key는 인코딩된 key보다 길지 않습니다. 0바이트 malloc을 피하려고 1을 더합니다. */
    char *key = malloc(encoded_key_len + 1);
    if (key == NULL) {
        return response_error(500, ERROR_BODY("out_of_memory"));
    }
    size_t key_len = 0;
    struct api_response response = response_unhandled();
    if (key_decode(encoded_key, encoded_key_len, key, &key_len)) {
        response = key_dispatch(store, method, method_len, key, key_len, body,
                                body_len);
    }
    /* store는 key를 복사해서 가지므로, 응답을 돌려주기 전에 풀어도 됩니다. */
    free(key);

    return response;
}

static struct api_response key_dispatch(struct store *store, const char *method,
                                        size_t method_len, const char *key,
                                        size_t key_len, const void *body,
                                        size_t body_len)
{
    if (method_is(method, method_len, "GET")) {
        return key_get(store, key, key_len);
    }
    if (method_is(method, method_len, "PUT")) {
        return key_set(store, key, key_len, body, body_len);
    }
    if (method_is(method, method_len, "DELETE")) {
        return key_delete(store, key, key_len);
    }

    return response_unhandled();
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

static bool method_is(const char *method, size_t method_len, const char *name)
{
    return method_len == strlen(name) && memcmp(method, name, method_len) == 0;
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

/* 이 티켓이 아직 다루지 않는 요청입니다. 뒤 티켓이 오류 응답으로 바꿉니다. */
static struct api_response response_unhandled(void)
{
    struct api_response response = {501, NULL, NULL, 0};
    return response;
}
