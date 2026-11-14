#include <stdbool.h>
#include <string.h>

#include "acutest.h"
#include "api.h"

/* 만료와 상관없는 테스트가 넘기는 "지금"입니다. 모든 key를 만료 없이 저장하므로
   어떤 시각이어도 결과가 같습니다. */
#define ANY_NOW 0

/* C 문자열 리터럴로 메서드와 경로를 넘깁니다. body는 주소와 길이로 받습니다. */
static struct api_response request(struct store *store, const char *method,
                                   const char *path, const void *body,
                                   size_t body_len)
{
    struct api_ttl_header no_ttl_header = {.received_at = ANY_NOW};
    return api_handle(store, ANY_NOW, method, strlen(method), path,
                      strlen(path), &no_ttl_header, body, body_len);
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

static void empty_value_returns_200_and_empty_body(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    struct api_response saved = request(store, "PUT", "/keys/empty", NULL, 0);
    TEST_CHECK(saved.status == 204);

    struct api_response response =
        request(store, "GET", "/keys/empty", NULL, 0);

    TEST_CHECK(response.status == 200);
    TEST_MSG("상태 코드: %d", response.status);
    TEST_CHECK(response.body_len == 0);
    TEST_MSG("body 길이: %zu", response.body_len);
    store_destroy(store);
}

static void content_type_is_octet_stream_for_get_and_json_for_error(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    request(store, "PUT", "/keys/name", "jungle", 6);

    struct api_response found = request(store, "GET", "/keys/name", NULL, 0);
    struct api_response missing = request(store, "GET", "/keys/none", NULL, 0);

    TEST_ASSERT(found.content_type != NULL);
    TEST_CHECK(strcmp(found.content_type, "application/octet-stream") == 0);
    TEST_MSG("조회의 Content-Type: %s", found.content_type);
    TEST_ASSERT(missing.content_type != NULL);
    TEST_CHECK(strcmp(missing.content_type, "application/json") == 0);
    TEST_MSG("오류의 Content-Type: %s", missing.content_type);
    store_destroy(store);
}

static void encoded_slash_becomes_slash_byte_in_key(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    struct api_response saved = request(store, "PUT", "/keys/a%2Fb", "v", 1);
    TEST_CHECK(saved.status == 204);

    /* 인코딩을 풀지 않은 /도 key의 바이트이므로, 같은 key "a/b"입니다. */
    struct api_response response = request(store, "GET", "/keys/a/b", NULL, 0);

    TEST_CHECK(response.status == 200);
    TEST_MSG("상태 코드: %d", response.status);
    TEST_ASSERT(response.body_len == 1);
    TEST_CHECK(memcmp(response.body, "v", 1) == 0);
    store_destroy(store);
}

static void encoded_nul_key_is_not_truncated(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    struct api_response saved = request(store, "PUT", "/keys/a%00b", "v", 1);
    TEST_CHECK(saved.status == 204);

    struct api_response found = request(store, "GET", "/keys/a%00b", NULL, 0);
    /* '\0'에서 잘렸다면 key "a"로 저장됐을 것입니다. */
    struct api_response truncated = request(store, "GET", "/keys/a", NULL, 0);

    TEST_CHECK(found.status == 200);
    TEST_MSG("a%%00b 조회의 상태 코드: %d", found.status);
    TEST_ASSERT(found.body_len == 1);
    TEST_CHECK(memcmp(found.body, "v", 1) == 0);
    expect_error(truncated, 404, "{\"error\":\"key_not_found\"}");
    store_destroy(store);
}

static void encoded_space_becomes_space_byte_in_key(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    request(store, "PUT", "/keys/a%20b", "v", 1);

    struct api_response response = request(store, "GET", "/keys/a b", NULL, 0);

    TEST_CHECK(response.status == 200);
    TEST_MSG("상태 코드: %d", response.status);
    store_destroy(store);
}

static void percent_encoding_is_decoded_only_once(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    request(store, "PUT", "/keys/%2541", "v", 1);

    /* 두 번 풀었다면 key는 "A"입니다. */
    struct api_response twice = request(store, "GET", "/keys/A", NULL, 0);
    struct api_response once = request(store, "GET", "/keys/%2541", NULL, 0);

    expect_error(twice, 404, "{\"error\":\"key_not_found\"}");
    TEST_CHECK(once.status == 200);
    TEST_MSG("%%2541 조회의 상태 코드: %d", once.status);
    store_destroy(store);
}

static void encoded_hangul_becomes_utf8_bytes_in_key(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    request(store, "PUT", "/keys/%ED%95%9C", "v", 1);

    /* "한"의 UTF-8 바이트 ED 95 9C를 인코딩 없이 보낸 경로입니다. */
    struct api_response response =
        request(store, "GET", "/keys/\xED\x95\x9C", NULL, 0);

    TEST_CHECK(response.status == 200);
    TEST_MSG("상태 코드: %d", response.status);
    store_destroy(store);
}

static void empty_key_returns_400_empty_key(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response response = request(store, "PUT", "/keys/", "v", 1);

    expect_error(response, 400, "{\"error\":\"empty_key\"}");
    store_destroy(store);
}

static void broken_encoding_returns_400_bad_key_encoding(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response not_hex = request(store, "GET", "/keys/%G1", NULL, 0);
    struct api_response cut_short = request(store, "GET", "/keys/a%2", NULL, 0);

    expect_error(not_hex, 400, "{\"error\":\"bad_key_encoding\"}");
    expect_error(cut_short, 400, "{\"error\":\"bad_key_encoding\"}");
    store_destroy(store);
}

static void raw_question_mark_returns_400_bad_key_encoding(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response with_query =
        request(store, "PUT", "/keys/a?b=1", "v", 1);
    struct api_response empty_query = request(store, "PUT", "/keys/a?", "v", 1);
    /* ? 앞까지를 key로 읽었다면 key "a"가 저장됐을 것입니다. */
    struct api_response stored = request(store, "GET", "/keys/a", NULL, 0);

    expect_error(with_query, 400, "{\"error\":\"bad_key_encoding\"}");
    expect_error(empty_query, 400, "{\"error\":\"bad_key_encoding\"}");
    expect_error(stored, 404, "{\"error\":\"key_not_found\"}");
    store_destroy(store);
}

static void encoded_question_mark_becomes_question_mark_byte_in_key(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    struct api_response saved = request(store, "PUT", "/keys/a%3Fb", "v", 1);
    TEST_CHECK(saved.status == 204);

    /* 소문자 16진수 %3f도 같은 ? 바이트이므로, 같은 key "a?b"입니다. */
    struct api_response response =
        request(store, "GET", "/keys/a%3fb", NULL, 0);

    TEST_CHECK(response.status == 200);
    TEST_MSG("상태 코드: %d", response.status);
    TEST_ASSERT(response.body_len == 1);
    TEST_CHECK(memcmp(response.body, "v", 1) == 0);
    store_destroy(store);
}

static void path_outside_keys_returns_404_route_not_found(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response response = request(store, "GET", "/nope", NULL, 0);

    expect_error(response, 404, "{\"error\":\"route_not_found\"}");
    store_destroy(store);
}

static void keys_without_trailing_slash_returns_404_route_not_found(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response response = request(store, "GET", "/keys", NULL, 0);

    expect_error(response, 404, "{\"error\":\"route_not_found\"}");
    store_destroy(store);
}

static void post_to_key_returns_405_method_not_allowed(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response response = request(store, "POST", "/keys/name", "v", 1);

    expect_error(response, 405, "{\"error\":\"method_not_allowed\"}");
    store_destroy(store);
}

static void method_not_allowed_lists_allowed_methods(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response response =
        request(store, "PATCH", "/keys/name", "v", 1);

    TEST_CHECK(response.status == 405);
    TEST_ASSERT(response.allow != NULL);
    TEST_CHECK(strcmp(response.allow, "GET, PUT, DELETE") == 0);
    TEST_MSG("Allow: %s", response.allow);
    store_destroy(store);
}

static void wrong_path_and_method_returns_404_route_not_found(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response response = request(store, "POST", "/nope", NULL, 0);

    expect_error(response, 404, "{\"error\":\"route_not_found\"}");
    store_destroy(store);
}

static void wrong_method_and_empty_key_returns_405(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response response = request(store, "POST", "/keys/", NULL, 0);

    expect_error(response, 405, "{\"error\":\"method_not_allowed\"}");
    store_destroy(store);
}

static void body_one_byte_over_limit_returns_413_body_too_large(void)
{
    struct api_response response = {0};

    /* 상한 16 MiB(16,777,216바이트)보다 1바이트 큽니다. */
    bool rejected = api_reject_headers(false, 16777217, &response);

    TEST_CHECK(rejected);
    expect_error(response, 413, "{\"error\":\"body_too_large\"}");
}

static void body_at_limit_is_accepted(void)
{
    /* 거절하지 않으면 response를 건드리지 않으므로, 표시값이 남아야 합니다. */
    struct api_response response = {.status = -1};

    bool rejected = api_reject_headers(false, 16777216, &response);

    TEST_CHECK(!rejected);
    TEST_CHECK(response.status == -1);
    TEST_MSG("상태 코드: %d", response.status);
}

static void transfer_encoding_returns_411_length_required(void)
{
    struct api_response response = {0};

    bool rejected = api_reject_headers(true, 0, &response);

    TEST_CHECK(rejected);
    expect_error(response, 411, "{\"error\":\"length_required\"}");
}

TEST_LIST = {
    {"[T9] 저장한 key를 조회하면 200과 저장한 value를 돌려준다",
     get_after_put_returns_200_and_value},
    {"저장에 성공하면 새로 만들었든 덮어썼든 204를 돌려준다",
     put_returns_204_for_new_and_overwrite},
    {"없는 key를 조회하면 404와 {\"error\":\"key_not_found\"}를 돌려준다",
     get_missing_key_returns_404_key_not_found},
    {"[T10] 있는 key를 지우면 204를, 없는 key를 지우면 404와 "
     "{\"error\":\"key_not_found\"}를 돌려준다",
     delete_returns_204_or_404_key_not_found},
    {"0바이트 value를 저장하고 조회하면 200과 0바이트 body를 돌려준다",
     empty_value_returns_200_and_empty_body},
    {"조회 응답의 Content-Type은 application/octet-stream이고, 오류 응답은 "
     "application/json이다",
     content_type_is_octet_stream_for_get_and_json_for_error},
    {"%2F는 경로 구분자가 아니라 key 안의 / 바이트가 된다",
     encoded_slash_becomes_slash_byte_in_key},
    {"%00이 든 key를 잘리지 않고 저장하고 조회한다",
     encoded_nul_key_is_not_truncated},
    {"%20이 든 key를 공백 바이트 그대로 key로 쓴다",
     encoded_space_becomes_space_byte_in_key},
    {"%2541은 한 번만 풀려 %41이라는 key가 된다",
     percent_encoding_is_decoded_only_once},
    {"한글을 인코딩한 key를 그 UTF-8 바이트 그대로 key로 쓴다",
     encoded_hangul_becomes_utf8_bytes_in_key},
    {"/keys/처럼 key가 비면 400과 {\"error\":\"empty_key\"}를 돌려준다",
     empty_key_returns_400_empty_key},
    {"%G1이나 끝이 잘린 %2는 400과 {\"error\":\"bad_key_encoding\"}을 돌려준다",
     broken_encoding_returns_400_bad_key_encoding},
    {"경로에 인코딩하지 않은 ?가 있으면 400과 "
     "{\"error\":\"bad_key_encoding\"}을 돌려주고 저장하지 않는다",
     raw_question_mark_returns_400_bad_key_encoding},
    {"%3F는 key 안의 ? 바이트가 된다",
     encoded_question_mark_becomes_question_mark_byte_in_key},
    {"/keys/로 시작하지 않는 경로는 404와 {\"error\":\"route_not_found\"}를 "
     "돌려준다",
     path_outside_keys_returns_404_route_not_found},
    {"끝에 /가 없는 /keys는 404와 route_not_found를 돌려준다",
     keys_without_trailing_slash_returns_404_route_not_found},
    {"/keys/<key>에 POST를 보내면 405와 {\"error\":\"method_not_allowed\"}를 "
     "돌려준다",
     post_to_key_returns_405_method_not_allowed},
    {"405 응답은 Allow 헤더로 GET, PUT, DELETE를 알린다",
     method_not_allowed_lists_allowed_methods},
    {"[T11] 경로와 메서드가 모두 틀리면 경로 오류인 404와 route_not_found를 "
     "돌려준다",
     wrong_path_and_method_returns_404_route_not_found},
    {"메서드가 틀리고 key가 비면 메서드 오류인 405를 돌려준다",
     wrong_method_and_empty_key_returns_405},
    {"Content-Length가 상한보다 1바이트 크면 413과 "
     "{\"error\":\"body_too_large\"}를 돌려준다",
     body_one_byte_over_limit_returns_413_body_too_large},
    {"Content-Length가 상한과 같으면 받아들인다", body_at_limit_is_accepted},
    {"Transfer-Encoding으로 body 길이를 알리면 411과 "
     "{\"error\":\"length_required\"}를 돌려준다",
     transfer_encoding_returns_411_length_required},
    {NULL, NULL},
};
