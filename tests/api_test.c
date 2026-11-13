#include <string.h>

#include "acutest.h"
#include "api.h"

/* C 문자열 리터럴로 메서드와 경로를 넘깁니다. body는 주소와 길이로 받습니다. */
static struct api_response request(struct store *store, const char *method,
                                   const char *path, const void *body,
                                   size_t body_len)
{
    return api_handle(store, method, strlen(method), path, strlen(path), body,
                      body_len);
}

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

static void get_after_put_returns_200_and_value(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    request(store, "PUT", "/keys/name", "jungle", 6);

    struct api_response response = request(store, "GET", "/keys/name", NULL, 0);

    TEST_CHECK(response.status == 200);
    TEST_MSG("상태 코드: %d", response.status);
    TEST_ASSERT(response.content_type != NULL);
    TEST_CHECK(strcmp(response.content_type, "application/octet-stream") == 0);
    TEST_ASSERT(response.body_len == 6);
    TEST_CHECK(memcmp(response.body, "jungle", 6) == 0);
    store_destroy(store);
}

static void put_returns_204_for_new_and_overwrite(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response created = request(store, "PUT", "/keys/name", "a", 1);
    struct api_response overwritten =
        request(store, "PUT", "/keys/name", "bb", 2);

    TEST_CHECK(created.status == 204);
    TEST_MSG("새로 만든 저장의 상태 코드: %d", created.status);
    TEST_CHECK(created.body_len == 0);
    TEST_CHECK(overwritten.status == 204);
    TEST_MSG("덮어쓴 저장의 상태 코드: %d", overwritten.status);
    TEST_CHECK(overwritten.body_len == 0);
    store_destroy(store);
}

static void get_missing_key_returns_404_key_not_found(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response response = request(store, "GET", "/keys/none", NULL, 0);

    expect_error(response, 404, "{\"error\":\"key_not_found\"}");
    store_destroy(store);
}

static void delete_returns_204_or_404_key_not_found(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    request(store, "PUT", "/keys/name", "jungle", 6);

    struct api_response deleted =
        request(store, "DELETE", "/keys/name", NULL, 0);
    struct api_response missing =
        request(store, "DELETE", "/keys/name", NULL, 0);

    TEST_CHECK(deleted.status == 204);
    TEST_MSG("있는 key 삭제의 상태 코드: %d", deleted.status);
    TEST_CHECK(deleted.body_len == 0);
    expect_error(missing, 404, "{\"error\":\"key_not_found\"}");
    store_destroy(store);
}

TEST_LIST = {
    {"저장한 key를 조회하면 200과 저장한 value를 돌려준다",
     get_after_put_returns_200_and_value},
    {"저장에 성공하면 새로 만들었든 덮어썼든 204를 돌려준다",
     put_returns_204_for_new_and_overwrite},
    {"없는 key를 조회하면 404와 {\"error\":\"key_not_found\"}를 돌려준다",
     get_missing_key_returns_404_key_not_found},
    {"있는 key를 지우면 204를, 없는 key를 지우면 404와 "
     "{\"error\":\"key_not_found\"}를 돌려준다",
     delete_returns_204_or_404_key_not_found},
    {NULL, NULL},
};
