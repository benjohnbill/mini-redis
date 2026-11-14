#include <stdio.h>
#include <string.h>

#include "acutest.h"
#include "api.h"
#include "store.h"

/* 버킷이 여러 번 늘었다가 줄어들 만큼의 key 수입니다. */
#define MANY_KEY_COUNT 20000
#define KEPT_KEY_COUNT 1000
/* 같은 key를 저장하고 지우기를 번갈아 하는 횟수입니다. */
#define BOUNDARY_ROUND_COUNT 100
#define KEY_TEXT_MAX 32

/* 오류 응답이 상태 코드, application/json, 기대한 body인지 확인합니다. */
static void expect_error(struct api_response response, int status,
                         const char *body)
{
    TEST_CHECK(response.status == status);
    TEST_MSG("상태 코드: %d", response.status);
    TEST_ASSERT(response.content_type != NULL);
    TEST_CHECK(strcmp(response.content_type, "application/json") == 0);
    TEST_MSG("Content-Type: %s", response.content_type);
    TEST_ASSERT(response.body_len == strlen(body));
    TEST_CHECK(memcmp(response.body, body, response.body_len) == 0);
}

static void request_timeout_returns_408_request_timeout(void)
{
    struct api_response response = api_reject_timeout();

    expect_error(response, 408, "{\"error\":\"request_timeout\"}");
}

static void too_many_connections_returns_503_json(void)
{
    struct api_response response = api_reject_connection();

    expect_error(response, 503, "{\"error\":\"too_many_connections\"}");
}

/* key-<index> 형태의 key를 key_text에 쓰고 그 길이를 돌려줍니다. */
static size_t key_format(char *key_text, int index)
{
    return (size_t)snprintf(key_text, KEY_TEXT_MAX, "key-%d", index);
}

static void store_put_index(struct store *store, int index)
{
    char key_text[KEY_TEXT_MAX];
    size_t key_len = key_format(key_text, index);

    TEST_ASSERT(store_set(store, key_text, key_len, &index, sizeof index));
}

/* index의 key가 있는지 보고, 있으면 저장한 value(index 자신)인지 확인합니다. */
static bool store_has_index(struct store *store, int index)
{
    char key_text[KEY_TEXT_MAX];
    size_t key_len = key_format(key_text, index);
    const void *value = NULL;
    size_t value_len = 0;
    if (!store_get(store, key_text, key_len, &value, &value_len)) {
        return false;
    }
    TEST_CHECK(value_len == sizeof index);
    TEST_CHECK(memcmp(value, &index, sizeof index) == 0);

    return true;
}

static void store_delete_index(struct store *store, int index)
{
    char key_text[KEY_TEXT_MAX];
    size_t key_len = key_format(key_text, index);

    TEST_ASSERT(store_delete(store, key_text, key_len));
}

static void keys_survive_growing_then_shrinking(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    for (int i = 0; i < MANY_KEY_COUNT; i++) {
        store_put_index(store, i);
    }
    for (int i = KEPT_KEY_COUNT; i < MANY_KEY_COUNT; i++) {
        store_delete_index(store, i);
    }
    for (int i = 0; i < MANY_KEY_COUNT; i++) {
        TEST_CHECK(store_has_index(store, i) == (i < KEPT_KEY_COUNT));
        TEST_MSG("key-%d", i);
    }
    store_destroy(store);
}

static void keys_survive_repeated_store_delete(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    for (int i = 0; i < MANY_KEY_COUNT; i++) {
        store_put_index(store, i);
    }
    for (int round = 0; round < BOUNDARY_ROUND_COUNT; round++) {
        store_put_index(store, MANY_KEY_COUNT);
        store_delete_index(store, MANY_KEY_COUNT);
    }
    for (int i = 0; i < MANY_KEY_COUNT; i++) {
        TEST_CHECK(store_has_index(store, i));
        TEST_MSG("key-%d", i);
    }
    TEST_CHECK(!store_has_index(store, MANY_KEY_COUNT));
    store_destroy(store);
}

TEST_LIST = {
    {"요청 시간 초과 응답은 408과 {\"error\":\"request_timeout\"}이고 "
     "Content-Type은 application/json이다",
     request_timeout_returns_408_request_timeout},
    {"연결 수 초과 응답은 503과 {\"error\":\"too_many_connections\"}이고 "
     "Content-Type은 application/json이다",
     too_many_connections_returns_503_json},
    {"key를 많이 넣었다가 대부분 지워도 남은 key를 모두 조회하고 지운 key는 "
     "조회되지 않는다",
     keys_survive_growing_then_shrinking},
    {"같은 key를 저장하고 지우기를 반복해도 다른 key를 모두 조회할 수 있다",
     keys_survive_repeated_store_delete},
    {NULL, NULL},
};
