#include "api.h"

#include <stdbool.h>
#include <string.h>

#define KEYS_PREFIX "/keys/"
#define KEYS_PREFIX_LEN (sizeof KEYS_PREFIX - 1)
/* 오류 응답의 body입니다. 리터럴을 이어 붙여 컴파일 때 문자열 하나가 됩니다. */
#define ERROR_BODY(reason) "{\"error\":\"" reason "\"}"

static bool method_is(const char *method, size_t method_len, const char *name);
static struct api_response key_get(struct store *store, const char *key,
                                   size_t key_len);
static struct api_response key_set(struct store *store, const char *key,
                                   size_t key_len, const void *value,
                                   size_t value_len);
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
    /* 퍼센트 인코딩은 아직 풀지 않고, `/keys/` 뒤의 나머지 전부를 key로 씁니다. */
    const char *key = path + KEYS_PREFIX_LEN;
    size_t key_len = path_len - KEYS_PREFIX_LEN;

    if (method_is(method, method_len, "GET")) {
        return key_get(store, key, key_len);
    }
    if (method_is(method, method_len, "PUT")) {
        return key_set(store, key, key_len, body, body_len);
    }

    return response_unhandled();
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
    /* store_set의 false는 메모리 부족뿐입니다. 오류 body는 뒤 티켓이 더합니다. */
    struct api_response response = {500, NULL, NULL, 0};
    if (store_set(store, key, key_len, value, value_len)) {
        response.status = 204;
    }

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
