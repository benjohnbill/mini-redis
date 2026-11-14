#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

/* 주기 청소 테스트의 한 번 청소량입니다. 서버의 K와 상관없이 테스트가 정합니다. */
#define SWEEP_MAX 20

static void store_set_expiring(struct store *store, const char *key,
                               uint64_t expires_at)
{
    TEST_ASSERT(store_set(store, key, strlen(key), "v", 1, expires_at));
}

/* 시각을 되돌려 물으므로, 만료와 상관없이 entry가 store에 남았는지 보입니다. */
static bool key_is_kept(struct store *store, const char *key)
{
    const void *value = NULL;
    size_t value_len = 0;
    return store_get(store, 0, key, strlen(key), &value, &value_len);
}

static void sweep_deletes_expired_keys_and_counts_them(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    store_set_expiring(store, "a", 1000);
    store_set_expiring(store, "b", 1000);

    TEST_CHECK(store_delete_expired(store, 1000, SWEEP_MAX) == 2);
    TEST_CHECK(!key_is_kept(store, "a"));
    TEST_CHECK(!key_is_kept(store, "b"));

    store_destroy(store);
}

static void sweep_deletes_at_most_max_count_per_call(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    store_set_expiring(store, "a", 1000);
    store_set_expiring(store, "b", 1000);
    store_set_expiring(store, "c", 1000);

    TEST_CHECK(store_delete_expired(store, 1000, 2) == 2);
    TEST_CHECK(store_delete_expired(store, 1000, 2) == 1);
    TEST_CHECK(!key_is_kept(store, "a"));
    TEST_CHECK(!key_is_kept(store, "b"));
    TEST_CHECK(!key_is_kept(store, "c"));

    store_destroy(store);
}

static void sweep_deletes_earliest_expiry_first(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    store_set_expiring(store, "late", 3000);
    store_set_expiring(store, "early", 1000);
    store_set_expiring(store, "middle", 2000);

    TEST_CHECK(store_delete_expired(store, 3000, 1) == 1);
    TEST_CHECK(!key_is_kept(store, "early"));
    TEST_CHECK(key_is_kept(store, "middle"));
    TEST_CHECK(store_delete_expired(store, 3000, 1) == 1);
    TEST_CHECK(!key_is_kept(store, "middle"));
    TEST_CHECK(key_is_kept(store, "late"));

    store_destroy(store);
}

static void sweep_keeps_live_keys_and_keys_without_expiry(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    store_set_expiring(store, "expired", 1000);
    store_set_expiring(store, "live", 1001);
    store_set_expiring(store, "forever", STORE_NO_EXPIRY);

    TEST_CHECK(store_delete_expired(store, 1000, SWEEP_MAX) == 1);
    TEST_CHECK(key_is_kept(store, "live"));
    TEST_CHECK(key_is_kept(store, "forever"));
    TEST_CHECK(store_delete_expired(store, FAR_FUTURE_NOW, SWEEP_MAX) == 1);
    TEST_CHECK(key_is_kept(store, "forever"));

    store_destroy(store);
}

static void sweep_skips_key_overwritten_without_expiry(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    store_set_expiring(store, "name", 1000);
    store_set_expiring(store, "name", STORE_NO_EXPIRY);

    TEST_CHECK(store_delete_expired(store, 1000, SWEEP_MAX) == 0);
    TEST_CHECK(key_is_kept(store, "name"));

    store_destroy(store);
}

static void sweep_does_not_count_deleted_key(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    store_set_expiring(store, "name", 1000);
    TEST_ASSERT(store_delete(store, 999, "name", 4));

    TEST_CHECK(store_delete_expired(store, 1000, SWEEP_MAX) == 0);

    store_destroy(store);
}

static void sweep_sees_only_last_expiry_of_overwritten_key(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    store_set_expiring(store, "name", 1000);
    store_set_expiring(store, "name", 3000);
    store_set_expiring(store, "name", 2000);

    TEST_CHECK(store_delete_expired(store, 1999, SWEEP_MAX) == 0);
    TEST_CHECK(key_is_kept(store, "name"));
    TEST_CHECK(store_delete_expired(store, 2000, SWEEP_MAX) == 1);
    TEST_CHECK(!key_is_kept(store, "name"));

    store_destroy(store);
}

static void sweep_deletes_nothing_without_expired_keys(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_CHECK(store_delete_expired(store, 1000, SWEEP_MAX) == 0);
    store_set_expiring(store, "name", 1001);

    TEST_CHECK(store_delete_expired(store, 1000, SWEEP_MAX) == 0);
    TEST_CHECK(key_is_kept(store, "name"));

    store_destroy(store);
}

/* 무작위 테스트의 key 수입니다. 만료 시각을 key마다 다르게 만들려고 1000보다
   작게 둡니다. */
#define RANDOM_KEY_COUNT 500
#define RANDOM_STEP_COUNT 5000
/* 테스트가 기억하는 "지금 store에 없음"입니다. 만료 시각으로 쓰이지 않는 0입니다. */
#define KEY_ABSENT 0

/* 실행마다 같은 순서를 내는 선형 합동 생성기입니다. */
static uint64_t random_next(uint64_t *state)
{
    *state = *state * 6364136223846793005u + 1442695040888963407u;
    return *state >> 33;
}

static void random_key_name(char *name, size_t name_size, size_t key_index)
{
    snprintf(name, name_size, "key%zu", key_index);
}

/* key_index를 끝 세 자리에 넣어, 같은 순간에 store에 있는 key들의 만료 시각이
   모두 다르게 합니다. 그래야 청소 순서를 하나로 기대할 수 있습니다. */
static uint64_t random_expiry(uint64_t *state, size_t key_index)
{
    if (random_next(state) % 4 == 0) {
        return STORE_NO_EXPIRY;
    }
    return 1000 + (random_next(state) % 100000) * 1000 + key_index;
}

/* 무작위로 저장, 덮어쓰기, 삭제를 하고, 테스트가 기대하는 만료 시각을
   expected에 적습니다. */
static void random_changes_apply(struct store *store, uint64_t *expected)
{
    uint64_t state = 42;
    char name[16];
    for (size_t step = 0; step < RANDOM_STEP_COUNT; step++) {
        size_t key_index = random_next(&state) % RANDOM_KEY_COUNT;
        random_key_name(name, sizeof name, key_index);
        if (random_next(&state) % 3 == 0) {
            store_delete(store, 1, name, strlen(name));
            expected[key_index] = KEY_ABSENT;
        } else {
            expected[key_index] = random_expiry(&state, key_index);
            store_set_expiring(store, name, expected[key_index]);
        }
    }
}

static int expiry_compare(const void *left, const void *right)
{
    uint64_t left_expiry = *(const uint64_t *)left;
    uint64_t right_expiry = *(const uint64_t *)right;
    return (left_expiry > right_expiry) - (left_expiry < right_expiry);
}

static void sweep_order_survives_random_changes(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    uint64_t expected[RANDOM_KEY_COUNT] = {0};
    random_changes_apply(store, expected);

    char name[16];
    uint64_t expiries[RANDOM_KEY_COUNT];
    size_t expiry_count = 0;
    for (size_t key_index = 0; key_index < RANDOM_KEY_COUNT; key_index++) {
        random_key_name(name, sizeof name, key_index);
        TEST_CHECK(key_is_kept(store, name) ==
                   (expected[key_index] != KEY_ABSENT));
        if (expected[key_index] != KEY_ABSENT &&
            expected[key_index] != STORE_NO_EXPIRY) {
            expiries[expiry_count++] = expected[key_index];
        }
    }
    qsort(expiries, expiry_count, sizeof *expiries, expiry_compare);
    /* 만료 시각마다 한 개씩 지워지고, 지워진 key는 그 시각의 key입니다. */
    for (size_t order = 0; order < expiry_count; order++) {
        random_key_name(name, sizeof name, expiries[order] % 1000);
        TEST_CHECK(store_delete_expired(store, expiries[order], SWEEP_MAX) ==
                   1);
        TEST_CHECK(!key_is_kept(store, name));
    }
    TEST_CHECK(store_delete_expired(store, FAR_FUTURE_NOW, SWEEP_MAX) == 0);

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
    {"주기 청소는 만료된 key를 지우고 지운 개수를 돌려준다",
     sweep_deletes_expired_keys_and_counts_them},
    {"주기 청소는 한 번에 K개보다 많이 지우지 않고, 남은 key는 다음 청소가 "
     "지운다",
     sweep_deletes_at_most_max_count_per_call},
    {"주기 청소는 만료 시각이 이른 key부터 지운다",
     sweep_deletes_earliest_expiry_first},
    {"주기 청소는 만료 전인 key와 기간이 없는 key를 지우지 않는다",
     sweep_keeps_live_keys_and_keys_without_expiry},
    {"[T17] 기간과 함께 저장한 key를 기간 없이 덮어쓰면 주기 청소가 그 key를 "
     "지우지 않는다",
     sweep_skips_key_overwritten_without_expiry},
    {"기간과 함께 저장한 key를 지우면 주기 청소가 지운 개수에 넣지 않는다",
     sweep_does_not_count_deleted_key},
    {"같은 key를 기간과 함께 여러 번 덮어써도 주기 청소는 마지막 기간만 본다",
     sweep_sees_only_last_expiry_of_overwritten_key},
    {"만료된 key가 없으면 주기 청소는 아무것도 지우지 않는다",
     sweep_deletes_nothing_without_expired_keys},
    {"기간이 있는 key를 많이 넣고 무작위로 지우고 덮어써도 남은 key를 모두 "
     "조회하고 만료 순서대로 청소된다",
     sweep_order_survives_random_changes},
    {NULL, NULL},
};
