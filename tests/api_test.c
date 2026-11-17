#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "acutest.h"
#include "api.h"

/* 만료와 상관없는 테스트가 넘기는 "지금"입니다. 모든 key를 만료 없이 저장하므로
   어떤 시각이어도 결과가 같습니다. */
#define ANY_NOW 0
/* 만료 경계 테스트에서 key가 만료되는 시각(ms)입니다. */
#define EXPIRES_AT 1000
/* key 한 줄 "k\n"의 바이트 수입니다. */
#define MGET_LINE_LEN 2
/* POST /mset 항목 "k\n1\nv\n"의 바이트 수입니다. */
#define MSET_ITEM_LEN 6

/* C 문자열 리터럴로 메서드와 경로를 넘깁니다. body는 주소와 길이로 받습니다. */
static struct api_response request(struct store *store, const char *method,
                                   const char *path, const void *body,
                                   size_t body_len)
{
    struct api_ttl_header no_ttl_header = {.received_at = ANY_NOW};
    return api_handle(store, ANY_NOW, method, strlen(method), path,
                      strlen(path), &no_ttl_header, body, body_len);
}

/* now 시각에 POST /mget을 보냅니다. */
static struct api_response mget(struct store *store, uint64_t now,
                                const void *body, size_t body_len)
{
    struct api_ttl_header no_ttl_header = {.received_at = now};
    return api_handle(store, now, "POST", strlen("POST"), "/mget",
                      strlen("/mget"), &no_ttl_header, body, body_len);
}

/* POST /mget의 200 응답이 기대한 바이트인지 확인하고, 응답이 가진 메모리를
   해제합니다. */
static void expect_mget_body(struct api_response *response, const void *body,
                             size_t body_len)
{
    TEST_CHECK(response->status == 200);
    TEST_MSG("상태 코드: %d", response->status);
    TEST_ASSERT(response->content_type != NULL);
    TEST_CHECK(strcmp(response->content_type, "application/octet-stream") == 0);
    TEST_CHECK(response->body_len == body_len);
    TEST_MSG("body 길이: 기대 %zu, 실제 %zu", body_len, response->body_len);
    TEST_CHECK(response->body_len == body_len &&
               memcmp(response->body, body, body_len) == 0);
    api_response_destroy(response);
}

/* now 시각에 POST /mset을 보냅니다. ttl이 NULL이면 TTL 헤더 없이 보냅니다. */
static struct api_response mset(struct store *store, uint64_t now,
                                const char *ttl, const void *body,
                                size_t body_len)
{
    struct api_ttl_header ttl_header = {.received_at = now};
    if (ttl != NULL) {
        ttl_header.text = ttl;
        ttl_header.text_len = strlen(ttl);
        ttl_header.count = 1;
    }
    return api_handle(store, now, "POST", strlen("POST"), "/mset",
                      strlen("/mset"), &ttl_header, body, body_len);
}

/* 저장 성공 응답(204, body 없음)인지 확인합니다. */
static void expect_no_content(struct api_response response)
{
    TEST_CHECK(response.status == 204);
    TEST_MSG("상태 코드: %d", response.status);
    TEST_CHECK(response.body_len == 0);
}

/* "k\n1\nv\n"을 item_count번 이어 붙인 POST /mset body를 만듭니다. 부른 쪽이
   해제합니다. */
static char *mset_body_create(size_t item_count)
{
    char *body = malloc(item_count * MSET_ITEM_LEN);
    for (size_t i = 0; body != NULL && i < item_count; i++) {
        memcpy(body + i * MSET_ITEM_LEN, "k\n1\nv\n", MSET_ITEM_LEN);
    }

    return body;
}

/* "k\n"을 line_count번 이어 붙인 body를 만듭니다. 부른 쪽이 해제합니다. */
static char *mget_body_create(size_t line_count)
{
    char *body = malloc(line_count * MGET_LINE_LEN);
    for (size_t i = 0; body != NULL && i < line_count; i++) {
        memcpy(body + i * MGET_LINE_LEN, "k\n", MGET_LINE_LEN);
    }

    return body;
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
    bool rejected = api_reject_headers(false, false, 16777217, &response);

    TEST_CHECK(rejected);
    expect_error(response, 413, "{\"error\":\"body_too_large\"}");
}

static void body_at_limit_is_accepted(void)
{
    /* 거절하지 않으면 response를 건드리지 않으므로, 표시값이 남아야 합니다. */
    struct api_response response = {.status = -1};

    bool rejected = api_reject_headers(false, false, 16777216, &response);

    TEST_CHECK(!rejected);
    TEST_CHECK(response.status == -1);
    TEST_MSG("상태 코드: %d", response.status);
}

static void transfer_encoding_returns_411_length_required(void)
{
    struct api_response response = {0};

    bool rejected = api_reject_headers(false, true, 0, &response);

    TEST_CHECK(rejected);
    expect_error(response, 411, "{\"error\":\"length_required\"}");
}

static void too_many_headers_returns_431_too_many_headers(void)
{
    struct api_response response = {0};

    bool rejected = api_reject_headers(true, false, 0, &response);

    TEST_CHECK(rejected);
    expect_error(response, 431, "{\"error\":\"too_many_headers\"}");
}

static void too_many_headers_wins_over_transfer_encoding_and_size(void)
{
    struct api_response response = {0};

    /* 넘친 헤더 뒤에 Transfer-Encoding과 Content-Length가 숨어 있을 수 있어서,
       411과 413보다 먼저 판정합니다. */
    bool rejected = api_reject_headers(true, true, 16777217, &response);

    TEST_CHECK(rejected);
    expect_error(response, 431, "{\"error\":\"too_many_headers\"}");
}

static void mget_returns_values_in_order_and_dash_for_missing(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    request(store, "PUT", "/keys/a", "1", 1);
    request(store, "PUT", "/keys/b", "22", 2);

    struct api_response response = mget(store, ANY_NOW, "b\nnone\na\n", 9);

    expect_mget_body(&response, "2\n22\n-\n1\n1\n", 11);
    store_destroy(store);
}

static void mget_last_newline_is_optional(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    request(store, "PUT", "/keys/a", "1", 1);

    struct api_response with_newline = mget(store, ANY_NOW, "a\nnone\n", 7);
    struct api_response without_newline = mget(store, ANY_NOW, "a\nnone", 6);

    expect_mget_body(&with_newline, "1\n1\n-\n", 6);
    expect_mget_body(&without_newline, "1\n1\n-\n", 6);
    store_destroy(store);
}

static void mget_empty_value_returns_zero_length(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    request(store, "PUT", "/keys/empty", NULL, 0);

    struct api_response response = mget(store, ANY_NOW, "empty", 5);

    expect_mget_body(&response, "0\n\n", 3);
    store_destroy(store);
}

static void mget_binary_value_keeps_newline_and_nul(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    request(store, "PUT", "/keys/bin", "x\ny\0z\n", 6);

    struct api_response response = mget(store, ANY_NOW, "bin\nbin", 7);

    expect_mget_body(&response, "6\nx\ny\0z\n\n6\nx\ny\0z\n\n", 18);
    store_destroy(store);
}

static void mget_duplicate_key_is_answered_each_time(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    request(store, "PUT", "/keys/a", "v", 1);

    struct api_response response = mget(store, ANY_NOW, "a\nnone\na\na", 10);

    expect_mget_body(&response, "1\nv\n-\n1\nv\n1\nv\n", 14);
    store_destroy(store);
}

static void mget_key_is_missing_from_its_expiry_time(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_ASSERT(store_set(store, "k", 1, "v", 1, EXPIRES_AT));

    struct api_response before = mget(store, EXPIRES_AT - 1, "k", 1);
    struct api_response at_expiry = mget(store, EXPIRES_AT, "k", 1);

    expect_mget_body(&before, "1\nv\n", 4);
    expect_mget_body(&at_expiry, "-\n", 2);
    store_destroy(store);
}

static void mget_newline_inside_key_is_sent_as_0a(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    request(store, "PUT", "/keys/two%0Alines", "v", 1);

    struct api_response response = mget(store, ANY_NOW, "two%0Alines", 11);

    expect_mget_body(&response, "1\nv\n", 4);
    store_destroy(store);
}

static void mget_accepts_64_keys_and_rejects_65(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    request(store, "PUT", "/keys/k", "v", 1);
    char *at_limit = mget_body_create(API_MGET_KEYS_MAX);
    char *over_limit = mget_body_create(API_MGET_KEYS_MAX + 1);
    char *expected = malloc(API_MGET_KEYS_MAX * 4);
    TEST_ASSERT(at_limit != NULL && over_limit != NULL && expected != NULL);
    for (size_t i = 0; i < API_MGET_KEYS_MAX; i++) {
        memcpy(expected + i * 4, "1\nv\n", 4);
    }

    struct api_response accepted =
        mget(store, ANY_NOW, at_limit, API_MGET_KEYS_MAX * MGET_LINE_LEN);
    struct api_response rejected = mget(
        store, ANY_NOW, over_limit, (API_MGET_KEYS_MAX + 1) * MGET_LINE_LEN);

    expect_mget_body(&accepted, expected, API_MGET_KEYS_MAX * 4);
    expect_error(rejected, 400, "{\"error\":\"too_many_keys\"}");
    free(expected);
    free(over_limit);
    free(at_limit);
    store_destroy(store);
}

static void mget_accepts_values_at_limit_and_rejects_one_byte_over(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    char *big = calloc(API_MGET_VALUES_MAX, 1);
    TEST_ASSERT(big != NULL);
    request(store, "PUT", "/keys/big", big, API_MGET_VALUES_MAX);
    request(store, "PUT", "/keys/small", "x", 1);
    request(store, "PUT", "/keys/empty", NULL, 0);

    /* 없는 key와 0바이트 value는 합계에 더하지 않습니다. */
    struct api_response at_limit = mget(store, ANY_NOW, "big\nnone\nempty", 14);
    struct api_response over_limit = mget(store, ANY_NOW, "big\nsmall", 9);

    TEST_CHECK(at_limit.status == 200);
    TEST_MSG("상한과 같은 합계의 상태 코드: %d", at_limit.status);
    size_t head_len = strlen("16777216\n");
    TEST_CHECK(at_limit.body_len == head_len + API_MGET_VALUES_MAX + 1 +
                                        strlen("-\n") + strlen("0\n\n"));
    TEST_CHECK(at_limit.body_len > head_len &&
               memcmp(at_limit.body, "16777216\n", head_len) == 0);
    api_response_destroy(&at_limit);
    expect_error(over_limit, 400, "{\"error\":\"response_too_large\"}");
    free(big);
    store_destroy(store);
}

static void mget_empty_line_returns_400_empty_key(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response middle = mget(store, ANY_NOW, "a\n\nb", 4);
    struct api_response only = mget(store, ANY_NOW, "\n", 1);

    expect_error(middle, 400, "{\"error\":\"empty_key\"}");
    expect_error(only, 400, "{\"error\":\"empty_key\"}");
    store_destroy(store);
}

static void mget_without_keys_returns_400_bad_request(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response response = mget(store, ANY_NOW, NULL, 0);

    expect_error(response, 400, "{\"error\":\"bad_request\"}");
    store_destroy(store);
}

static void mget_broken_encoding_returns_400_bad_key_encoding(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response broken = mget(store, ANY_NOW, "a\n%G1", 5);
    struct api_response raw_question_mark = mget(store, ANY_NOW, "a?b", 3);

    expect_error(broken, 400, "{\"error\":\"bad_key_encoding\"}");
    expect_error(raw_question_mark, 400, "{\"error\":\"bad_key_encoding\"}");
    store_destroy(store);
}

static void mget_too_many_keys_wins_over_broken_encoding(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    char *body = mget_body_create(API_MGET_KEYS_MAX + 1);
    TEST_ASSERT(body != NULL);
    body[0] = '%';

    struct api_response response =
        mget(store, ANY_NOW, body, (API_MGET_KEYS_MAX + 1) * MGET_LINE_LEN);

    expect_error(response, 400, "{\"error\":\"too_many_keys\"}");
    free(body);
    store_destroy(store);
}

static void mget_other_method_returns_405_with_allow_post(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response response = request(store, "GET", "/mget", NULL, 0);

    expect_error(response, 405, "{\"error\":\"method_not_allowed\"}");
    TEST_ASSERT(response.allow != NULL);
    TEST_CHECK(strcmp(response.allow, "POST") == 0);
    TEST_MSG("Allow: %s", response.allow);
    store_destroy(store);
}

static void mget_path_with_slash_or_query_returns_404_route_not_found(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response slash = request(store, "POST", "/mget/", "a", 1);
    struct api_response query = request(store, "POST", "/mget?x=1", "a", 1);

    expect_error(slash, 404, "{\"error\":\"route_not_found\"}");
    expect_error(query, 404, "{\"error\":\"route_not_found\"}");
    store_destroy(store);
}

static void mset_stores_every_item_and_returns_204(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response response =
        mset(store, ANY_NOW, NULL, "a\n1\n1\nb\n2\n22\n", 13);
    struct api_response stored = mget(store, ANY_NOW, "a\nb", 3);

    expect_no_content(response);
    expect_mget_body(&stored, "1\n1\n2\n22\n", 9);
    store_destroy(store);
}

static void mset_binary_value_keeps_newline_and_nul(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response response =
        mset(store, ANY_NOW, NULL, "bin\n6\nx\ny\0z\n\n", 13);
    struct api_response stored = mget(store, ANY_NOW, "bin", 3);

    expect_no_content(response);
    expect_mget_body(&stored, "6\nx\ny\0z\n\n", 9);
    store_destroy(store);
}

static void mset_duplicate_key_keeps_last_value(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response response =
        mset(store, ANY_NOW, NULL, "k\n1\na\nk\n1\nb\n", 12);
    struct api_response stored = mget(store, ANY_NOW, "k", 1);

    expect_no_content(response);
    expect_mget_body(&stored, "1\nb\n", 4);
    store_destroy(store);
}

static void mset_ttl_applies_to_every_key(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    /* TTL 1초를 시각 0에 받았으므로 두 key 모두 EXPIRES_AT에 만료됩니다. */
    struct api_response response =
        mset(store, 0, "1", "a\n1\n1\nb\n1\n2\n", 12);
    struct api_response before = mget(store, EXPIRES_AT - 1, "a\nb", 3);
    struct api_response at_expiry = mget(store, EXPIRES_AT, "a\nb", 3);

    expect_no_content(response);
    expect_mget_body(&before, "1\n1\n1\n2\n", 8);
    expect_mget_body(&at_expiry, "-\n-\n", 4);
    store_destroy(store);
}

static void mset_bad_ttl_returns_400_and_stores_nothing(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response response =
        mset(store, ANY_NOW, "0", "a\n1\n1\nb\n1\n2\n", 12);
    struct api_response stored = mget(store, ANY_NOW, "a\nb", 3);

    expect_error(response, 400, "{\"error\":\"bad_ttl\"}");
    expect_mget_body(&stored, "-\n-\n", 4);
    store_destroy(store);
}

static void mset_accepts_64_keys_and_rejects_65_without_storing(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    char *at_limit = mset_body_create(API_MSET_KEYS_MAX);
    char *over_limit = mset_body_create(API_MSET_KEYS_MAX + 1);
    TEST_ASSERT(at_limit != NULL && over_limit != NULL);

    struct api_response rejected =
        mset(store, ANY_NOW, NULL, over_limit,
             (API_MSET_KEYS_MAX + 1) * MSET_ITEM_LEN);
    struct api_response after_rejected = mget(store, ANY_NOW, "k", 1);
    struct api_response accepted =
        mset(store, ANY_NOW, NULL, at_limit, API_MSET_KEYS_MAX * MSET_ITEM_LEN);
    struct api_response after_accepted = mget(store, ANY_NOW, "k", 1);

    expect_error(rejected, 400, "{\"error\":\"too_many_keys\"}");
    expect_mget_body(&after_rejected, "-\n", 2);
    expect_no_content(accepted);
    expect_mget_body(&after_accepted, "1\nv\n", 4);
    free(over_limit);
    free(at_limit);
    store_destroy(store);
}

/* 앞 항목 "a\n1\n1\n"은 맞고 뒤 항목만 틀린 body들입니다. */
static void mset_malformed_later_item_returns_400_and_stores_nothing(void)
{
    static const char *const bodies[] = {
        "a\n1\n1\nb\n5\n22\n",  /* 길이가 남은 body보다 김 */
        "a\n1\n1\nb\n2\n22",    /* value 뒤의 \n이 없음 */
        "a\n1\n1\nb\n2x\n22\n", /* 길이에 숫자가 아닌 글자 */
        "a\n1\n1\nb\n\n22\n",   /* 길이가 빔 */
        "a\n1\n1\nb",           /* 길이 줄이 없음 */
    };
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    for (size_t i = 0; i < sizeof bodies / sizeof bodies[0]; i++) {
        struct api_response response =
            mset(store, ANY_NOW, NULL, bodies[i], strlen(bodies[i]));
        expect_error(response, 400, "{\"error\":\"bad_request\"}");
        TEST_MSG("body %zu번", i);
    }
    struct api_response stored = mget(store, ANY_NOW, "a", 1);

    expect_mget_body(&stored, "-\n", 2);
    store_destroy(store);
}

static void mset_bad_later_key_returns_400_and_stores_nothing(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response broken =
        mset(store, ANY_NOW, NULL, "a\n1\n1\n%G1\n1\n1\n", 14);
    struct api_response raw_question_mark =
        mset(store, ANY_NOW, NULL, "a\n1\n1\nb?c\n1\n1\n", 14);
    struct api_response empty =
        mset(store, ANY_NOW, NULL, "a\n1\n1\n\n1\n1\n", 11);
    struct api_response stored = mget(store, ANY_NOW, "a", 1);

    expect_error(broken, 400, "{\"error\":\"bad_key_encoding\"}");
    expect_error(raw_question_mark, 400, "{\"error\":\"bad_key_encoding\"}");
    expect_error(empty, 400, "{\"error\":\"empty_key\"}");
    expect_mget_body(&stored, "-\n", 2);
    store_destroy(store);
}

static void mset_without_items_returns_400_bad_request(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response response = mset(store, ANY_NOW, NULL, NULL, 0);

    expect_error(response, 400, "{\"error\":\"bad_request\"}");
    store_destroy(store);
}

static void mset_other_method_returns_405_with_allow_post(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response response = request(store, "PUT", "/mset", NULL, 0);

    expect_error(response, 405, "{\"error\":\"method_not_allowed\"}");
    TEST_ASSERT(response.allow != NULL);
    TEST_CHECK(strcmp(response.allow, "POST") == 0);
    TEST_MSG("Allow: %s", response.allow);
    store_destroy(store);
}

static void mset_path_with_slash_or_query_returns_404_route_not_found(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    struct api_response slash =
        request(store, "POST", "/mset/", "a\n1\n1\n", 6);
    struct api_response query =
        request(store, "POST", "/mset?x=1", "a\n1\n1\n", 6);

    expect_error(slash, 404, "{\"error\":\"route_not_found\"}");
    expect_error(query, 404, "{\"error\":\"route_not_found\"}");
    store_destroy(store);
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
    {"헤더가 상한보다 많으면 431과 {\"error\":\"too_many_headers\"}를 "
     "돌려준다",
     too_many_headers_returns_431_too_many_headers},
    {"헤더가 상한보다 많으면 Transfer-Encoding이나 큰 Content-Length가 있어도 "
     "431을 돌려준다",
     too_many_headers_wins_over_transfer_encoding_and_size},
    {"POST /mget은 key 순서대로 \"<길이>\\n<value>\\n\"을, 없는 key는 "
     "\"-\\n\"을 돌려준다",
     mget_returns_values_in_order_and_dash_for_missing},
    {"POST /mget body의 마지막 \\n은 없어도 같은 응답을 돌려준다",
     mget_last_newline_is_optional},
    {"POST /mget은 0바이트 value를 길이 0과 빈 value로 돌려준다",
     mget_empty_value_returns_zero_length},
    {"POST /mget은 \\n과 \\0이 든 value를 길이만큼 그대로 돌려준다",
     mget_binary_value_keeps_newline_and_nul},
    {"POST /mget에 같은 key가 여러 번 오면 나온 순서대로 매번 돌려준다",
     mget_duplicate_key_is_answered_each_time},
    {"POST /mget은 만료 시각 1 ms 전에는 value를, 만료 시각부터는 \"-\\n\"을 "
     "돌려준다",
     mget_key_is_missing_from_its_expiry_time},
    {"POST /mget에서 줄바꿈이 든 key는 %0A로 보낸다",
     mget_newline_inside_key_is_sent_as_0a},
    {"POST /mget은 key 64개를 받고 65개는 400 too_many_keys로 거절한다",
     mget_accepts_64_keys_and_rejects_65},
    {"POST /mget은 value 합계가 16 MiB면 받고 1바이트 넘으면 400 "
     "response_too_large로 거절한다",
     mget_accepts_values_at_limit_and_rejects_one_byte_over},
    {"POST /mget body의 빈 줄은 400 empty_key를 돌려준다",
     mget_empty_line_returns_400_empty_key},
    {"POST /mget body에 key가 없으면 400 bad_request를 돌려준다",
     mget_without_keys_returns_400_bad_request},
    {"POST /mget의 깨진 인코딩과 인코딩하지 않은 ?는 400 bad_key_encoding을 "
     "돌려준다",
     mget_broken_encoding_returns_400_bad_key_encoding},
    {"POST /mget은 key 개수 오류를 key 인코딩 오류보다 먼저 알린다",
     mget_too_many_keys_wins_over_broken_encoding},
    {"/mget에 POST가 아닌 메서드를 보내면 405와 Allow: POST를 돌려준다",
     mget_other_method_returns_405_with_allow_post},
    {"/mget/와 /mget?x=1은 404 route_not_found를 돌려준다",
     mget_path_with_slash_or_query_returns_404_route_not_found},
    {"POST /mset은 모든 항목을 저장하고 204를 돌려준다",
     mset_stores_every_item_and_returns_204},
    {"POST /mset은 \\n과 \\0이 든 value를 길이만큼 그대로 저장한다",
     mset_binary_value_keeps_newline_and_nul},
    {"POST /mset에 같은 key가 여러 번 오면 마지막 value가 남는다",
     mset_duplicate_key_keeps_last_value},
    {"POST /mset의 TTL은 모든 key에 같은 만료 시각을 붙인다",
     mset_ttl_applies_to_every_key},
    {"POST /mset의 TTL이 잘못되면 400 bad_ttl을 돌려주고 아무 key도 저장하지 "
     "않는다",
     mset_bad_ttl_returns_400_and_stores_nothing},
    {"POST /mset은 key 64개를 받고, 65개는 400 too_many_keys로 거절하며 아무 "
     "key도 저장하지 않는다",
     mset_accepts_64_keys_and_rejects_65_without_storing},
    {"POST /mset의 뒤 항목 모양이 틀리면 400 bad_request를 돌려주고 앞 key도 "
     "저장하지 않는다",
     mset_malformed_later_item_returns_400_and_stores_nothing},
    {"POST /mset의 뒤 key 인코딩이 틀리거나 비면 400을 돌려주고 앞 key도 "
     "저장하지 않는다",
     mset_bad_later_key_returns_400_and_stores_nothing},
    {"POST /mset body에 항목이 없으면 400 bad_request를 돌려준다",
     mset_without_items_returns_400_bad_request},
    {"/mset에 POST가 아닌 메서드를 보내면 405와 Allow: POST를 돌려준다",
     mset_other_method_returns_405_with_allow_post},
    {"/mset/와 /mset?x=1은 404 route_not_found를 돌려준다",
     mset_path_with_slash_or_query_returns_404_route_not_found},
    {NULL, NULL},
};
