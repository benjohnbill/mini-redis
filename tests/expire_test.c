#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "acutest.h"
#include "api.h"
#include "store.h"

/* "지금"은 mg_millis의 ms입니다. 서버가 시작한 지 약 5억 8천만 년 뒤의
   시각이므로, 서버가 실제로 만날 어떤 "지금"보다도 늦습니다. */
#define FAR_FUTURE_NOW (UINT64_MAX - 1)

static void key_without_expiry_is_found_at_far_future(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_CHECK(store_set(store, "name", 4, "jungle", 6, STORE_NO_EXPIRY));

    const void *value = NULL;
    size_t value_len = 0;
    TEST_ASSERT(
        store_get(store, FAR_FUTURE_NOW, "name", 4, &value, &value_len));
    TEST_CHECK(value_len == 6);
    TEST_CHECK(memcmp(value, "jungle", 6) == 0);

    store_destroy(store);
}

/* 만료 시각 EXPIRES_AT에 만료되는 key를 저장한 store입니다. */
#define EXPIRES_AT 1000

static struct store *store_with_name_expiring_at(uint64_t expires_at)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_ASSERT(store_set(store, "name", 4, "jungle", 6, expires_at));

    return store;
}

static bool name_is_found(struct store *store, uint64_t now)
{
    const void *value = NULL;
    size_t value_len = 0;
    return store_get(store, now, "name", 4, &value, &value_len);
}

static void key_expires_at_its_expiry_time(void)
{
    struct store *store = store_with_name_expiring_at(EXPIRES_AT);

    TEST_CHECK(name_is_found(store, EXPIRES_AT - 1));
    TEST_CHECK(!name_is_found(store, EXPIRES_AT));

    store_destroy(store);
}

static void deleting_expired_key_deletes_nothing(void)
{
    struct store *store = store_with_name_expiring_at(EXPIRES_AT);

    TEST_CHECK(!store_delete(store, EXPIRES_AT, "name", 4));
    /* 시각을 되돌려 물으면 entry가 실제로 빠졌는지 보입니다. */
    TEST_CHECK(!name_is_found(store, EXPIRES_AT - 1));

    store_destroy(store);
}

static void getting_expired_key_keeps_it_in_store(void)
{
    struct store *store = store_with_name_expiring_at(EXPIRES_AT);

    TEST_CHECK(!name_is_found(store, EXPIRES_AT));
    /* 시각을 되돌려 물으면 entry가 아직 store에 있는지 보입니다. */
    TEST_CHECK(name_is_found(store, EXPIRES_AT - 1));

    store_destroy(store);
}

static void overwrite_with_other_expiry_expires_at_new_time(void)
{
    struct store *store = store_with_name_expiring_at(EXPIRES_AT);
    TEST_ASSERT(store_set(store, "name", 4, "camp", 4, 5000));

    TEST_CHECK(name_is_found(store, EXPIRES_AT));
    TEST_CHECK(name_is_found(store, 4999));
    TEST_CHECK(!name_is_found(store, 5000));

    store_destroy(store);
}

static void overwrite_without_expiry_never_expires(void)
{
    struct store *store = store_with_name_expiring_at(EXPIRES_AT);
    TEST_ASSERT(store_set(store, "name", 4, "camp", 4, STORE_NO_EXPIRY));

    TEST_CHECK(name_is_found(store, EXPIRES_AT));
    TEST_CHECK(name_is_found(store, FAR_FUTURE_NOW));

    store_destroy(store);
}

/* 헤더를 headers_received_at에 다 받고 body까지 받아 now에 처리한 PUT입니다. */
static struct api_response put_with_ttl(struct store *store,
                                        uint64_t headers_received_at,
                                        uint64_t now, const char *ttl)
{
    struct api_ttl_header ttl_header = {.text = ttl,
                                        .text_len = strlen(ttl),
                                        .count = 1,
                                        .received_at = headers_received_at};
    return api_handle(store, now, "PUT", 3, "/keys/name", 10, &ttl_header,
                      "jungle", 6);
}

static int get_status(struct store *store, uint64_t now)
{
    struct api_ttl_header no_ttl_header = {.received_at = now};
    return api_handle(store, now, "GET", 3, "/keys/name", 10, &no_ttl_header,
                      NULL, 0)
        .status;
}

static void ttl_counts_from_headers_received(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    /* body를 받는 데 2초가 걸려도 기간은 헤더를 받은 1000 ms부터 셉니다. */
    TEST_CHECK(put_with_ttl(store, 1000, 3000, "60").status == 204);

    TEST_CHECK(get_status(store, 60999) == 200);
    TEST_CHECK(get_status(store, 61000) == 404);

    store_destroy(store);
}

static void get_does_not_push_back_expiry(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_CHECK(put_with_ttl(store, 1000, 1000, "1").status == 204);

    TEST_CHECK(get_status(store, 1500) == 200);
    TEST_CHECK(get_status(store, 1999) == 200);
    TEST_CHECK(get_status(store, 2000) == 404);

    store_destroy(store);
}

TEST_LIST = {
    {"만료 없이 저장한 key는 아주 먼 시각에 조회해도 value를 돌려준다",
     key_without_expiry_is_found_at_far_future},
    {"[T15] 만료 시각 1 ms 전에는 조회되고, 만료 시각과 같은 순간에는 없다",
     key_expires_at_its_expiry_time},
    {"만료된 key를 삭제하면 지운 것이 없다고 답한다",
     deleting_expired_key_deletes_nothing},
    {"만료된 key를 조회해도 store에서 지우지 않는다",
     getting_expired_key_keeps_it_in_store},
    {"기간과 함께 저장한 key를 다른 기간으로 덮어쓰면 새 기간이 지나야 "
     "만료된다",
     overwrite_with_other_expiry_expires_at_new_time},
    {"기간과 함께 저장한 key를 기간 없이 덮어쓰면 만료되지 않는다",
     overwrite_without_expiry_never_expires},
    {"TTL 60을 받으면 헤더를 받은 시각부터 60초 뒤가 만료 시각이다",
     ttl_counts_from_headers_received},
    {"기간이 남은 key를 조회해도 만료 시각이 늦춰지지 않는다",
     get_does_not_push_back_expiry},
    {NULL, NULL},
};
