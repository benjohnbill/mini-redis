#include <stdio.h>
#include <string.h>

#include "acutest.h"
#include "store.h"

static void set_then_get_returns_value_and_length(void) {
    struct store *s = store_create();
    TEST_ASSERT(s != NULL);

    TEST_CHECK(store_set(s, "name", 4, "jungle", 6));

    const void *value = NULL;
    size_t value_len = 0;
    TEST_CHECK(store_get(s, "name", 4, &value, &value_len));
    TEST_CHECK(value_len == 6);
    TEST_CHECK(memcmp(value, "jungle", 6) == 0);

    store_destroy(s);
}

static void get_missing_key_reports_not_found(void) {
    struct store *s = store_create();
    TEST_ASSERT(s != NULL);
    TEST_CHECK(store_set(s, "name", 4, "jungle", 6));

    const void *value = NULL;
    size_t value_len = 0;
    TEST_CHECK(!store_get(s, "age", 3, &value, &value_len));

    store_destroy(s);
}

static void empty_value_is_found_with_length_zero(void) {
    struct store *s = store_create();
    TEST_ASSERT(s != NULL);
    TEST_CHECK(store_set(s, "empty", 5, NULL, 0));

    const void *value = NULL;
    size_t value_len = 99;
    TEST_CHECK(store_get(s, "empty", 5, &value, &value_len));
    TEST_CHECK(value_len == 0);

    store_destroy(s);
}

static void empty_key_can_be_set_and_get(void) {
    struct store *s = store_create();
    TEST_ASSERT(s != NULL);
    TEST_CHECK(store_set(s, NULL, 0, "v", 1));

    const void *value = NULL;
    size_t value_len = 0;
    TEST_CHECK(store_get(s, NULL, 0, &value, &value_len));
    TEST_CHECK(value_len == 1);
    TEST_CHECK(memcmp(value, "v", 1) == 0);

    store_destroy(s);
}

static void value_with_inner_nul_is_not_truncated(void) {
    struct store *s = store_create();
    TEST_ASSERT(s != NULL);
    const char bin[5] = {'a', '\0', 'b', '\0', 'c'};
    TEST_CHECK(store_set(s, "bin", 3, bin, 5));

    const void *value = NULL;
    size_t value_len = 0;
    TEST_CHECK(store_get(s, "bin", 3, &value, &value_len));
    TEST_CHECK(value_len == 5);
    TEST_CHECK(memcmp(value, "a\0b\0c", 5) == 0);

    store_destroy(s);
}

static void changing_caller_buffer_after_set_keeps_stored_value(void) {
    struct store *s = store_create();
    TEST_ASSERT(s != NULL);
    char key[] = "name";
    char val[] = "jungle";
    TEST_CHECK(store_set(s, key, 4, val, 6));
    memcpy(val, "XXXXXX", 6);

    const void *value = NULL;
    size_t value_len = 0;
    TEST_CHECK(store_get(s, "name", 4, &value, &value_len));
    TEST_CHECK(value_len == 6);
    TEST_CHECK(memcmp(value, "jungle", 6) == 0);

    /* 호출한 쪽 key 버퍼를 바꿔도 저장된 key로 찾을 수 있어야 합니다. */
    memcpy(key, "XXXX", 4);
    TEST_CHECK(store_get(s, "name", 4, &value, &value_len));

    store_destroy(s);
}

static void keys_sharing_prefix_with_different_length_are_distinct(void) {
    struct store *s = store_create();
    TEST_ASSERT(s != NULL);
    TEST_CHECK(store_set(s, "abc", 3, "short", 5));
    TEST_CHECK(store_set(s, "abcd", 4, "long", 4));

    const void *value = NULL;
    size_t value_len = 0;
    TEST_CHECK(store_get(s, "abc", 3, &value, &value_len));
    TEST_CHECK(value_len == 5);
    TEST_CHECK(memcmp(value, "short", 5) == 0);
    TEST_CHECK(store_get(s, "abcd", 4, &value, &value_len));
    TEST_CHECK(value_len == 4);
    TEST_CHECK(memcmp(value, "long", 4) == 0);

    /* 두 key가 다른 버킷에 가면 비교가 일어나지 않습니다. "a", "aa", ... 64개는
       서로 앞부분이 같고, 버킷 수보다 많아서 같은 버킷에서 비교되는 쌍이 생깁니다.
       value에는 key의 길이를 1바이트로 적습니다. */
    char key[64];
    memset(key, 'a', sizeof key);
    for (size_t len = 1; len <= sizeof key; len++) {
        unsigned char v = (unsigned char)len;
        TEST_ASSERT(store_set(s, key, len, &v, 1));
    }
    for (size_t len = 1; len <= sizeof key; len++) {
        if (!TEST_CHECK(store_get(s, key, len, &value, &value_len))) {
            TEST_MSG("못 찾은 key 길이: %zu", len);
            break;
        }
        TEST_CHECK(value_len == 1);
        TEST_CHECK(*(const unsigned char *)value == len);
    }

    store_destroy(s);
}

/* 수천 개면 hash.c의 rehash가 여러 번 일어납니다. */
#define MANY_KEYS 5000

static void all_keys_found_after_storing_thousands(void) {
    struct store *s = store_create();
    TEST_ASSERT(s != NULL);
    char key[16];
    char val[16];
    for (int i = 0; i < MANY_KEYS; i++) {
        int klen = snprintf(key, sizeof key, "k%d", i);
        int vlen = snprintf(val, sizeof val, "v%d", i);
        TEST_ASSERT(store_set(s, key, (size_t)klen, val, (size_t)vlen));
    }

    for (int i = 0; i < MANY_KEYS; i++) {
        int klen = snprintf(key, sizeof key, "k%d", i);
        int vlen = snprintf(val, sizeof val, "v%d", i);
        const void *value = NULL;
        size_t value_len = 0;
        if (!TEST_CHECK(store_get(s, key, (size_t)klen, &value, &value_len))) {
            TEST_MSG("못 찾은 key: %s", key);
            break;
        }
        TEST_CHECK(value_len == (size_t)vlen);
        TEST_CHECK(memcmp(value, val, (size_t)vlen) == 0);
    }

    store_destroy(s);
}

/* entry가 남은 채로 없애도 AddressSanitizer(LeakSanitizer)가 누수를 보고하지
   않아야 합니다. acutest는 테스트마다 자식 프로세스를 exit()로 끝내므로, 누수가
   있으면 이 테스트가 실패합니다. */
static void destroy_leaves_no_leak(void) {
    struct store *s = store_create();
    TEST_ASSERT(s != NULL);
    TEST_CHECK(store_set(s, "a", 1, "1", 1));
    TEST_CHECK(store_set(s, NULL, 0, NULL, 0));
    TEST_CHECK(store_set(s, "b", 1, "22", 2));
    store_destroy(s);
}

static void set_twice_returns_later_value(void) {
    struct store *s = store_create();
    TEST_ASSERT(s != NULL);

    TEST_CHECK(store_set(s, "name", 4, "jungle", 6));
    TEST_CHECK(store_set(s, "name", 4, "krafto", 6));

    const void *value = NULL;
    size_t value_len = 0;
    TEST_CHECK(store_get(s, "name", 4, &value, &value_len));
    TEST_CHECK(value_len == 6);
    TEST_CHECK(memcmp(value, "krafto", 6) == 0);

    store_destroy(s);
}

static void overwrite_with_different_length_returns_new_length(void) {
    struct store *s = store_create();
    TEST_ASSERT(s != NULL);

    TEST_CHECK(store_set(s, "name", 4, "jungle", 6));
    TEST_CHECK(store_set(s, "name", 4, "ab", 2));

    const void *value = NULL;
    size_t value_len = 0;
    TEST_CHECK(store_get(s, "name", 4, &value, &value_len));
    TEST_CHECK(value_len == 2);
    TEST_CHECK(memcmp(value, "ab", 2) == 0);

    TEST_CHECK(store_set(s, "name", 4, "longer value", 12));
    TEST_CHECK(store_get(s, "name", 4, &value, &value_len));
    TEST_CHECK(value_len == 12);
    TEST_CHECK(memcmp(value, "longer value", 12) == 0);

    store_destroy(s);
}

/* 덮어쓰면 밀려난 옛 entry를 store가 해제해야 합니다. 해제하지 않으면 테이블에서
   빠진 옛 entry를 가리키는 곳이 없어 store_destroy도 해제하지 못하고,
   LeakSanitizer가 자식 프로세스 종료 때 누수로 보고합니다. */
static void overwrite_leaves_no_leak_of_old_entry(void) {
    struct store *s = store_create();
    TEST_ASSERT(s != NULL);
    TEST_CHECK(store_set(s, "name", 4, "jungle", 6));
    TEST_CHECK(store_set(s, "name", 4, "krafton", 7));
    TEST_CHECK(store_set(s, "name", 4, "x", 1));
    store_destroy(s);
}

static void delete_existing_key_reports_removed_and_get_fails(void) {
    struct store *s = store_create();
    TEST_ASSERT(s != NULL);
    TEST_CHECK(store_set(s, "name", 4, "jungle", 6));

    TEST_CHECK(store_delete(s, "name", 4));

    const void *value = NULL;
    size_t value_len = 0;
    TEST_CHECK(!store_get(s, "name", 4, &value, &value_len));

    store_destroy(s);
}

TEST_LIST = {
    {"[T1] 저장한 key를 조회하면 저장한 value와 길이를 돌려준다", set_then_get_returns_value_and_length},
    {"[T2] 없는 key를 조회하면 찾지 못했다고 알린다", get_missing_key_reports_not_found},
    {"0바이트 value를 저장하면 찾았고 길이가 0이라고 알린다", empty_value_is_found_with_length_zero},
    {"0바이트 key도 저장하고 조회할 수 있다", empty_key_can_be_set_and_get},
    {"중간에 '\\0'이 있는 value를 잘리지 않고 돌려준다", value_with_inner_nul_is_not_truncated},
    {"저장한 뒤 호출한 쪽 버퍼를 바꿔도 저장된 value는 그대로다", changing_caller_buffer_after_set_keeps_stored_value},
    {"앞부분이 같고 길이가 다른 key를 서로 다른 key로 다룬다", keys_sharing_prefix_with_different_length_are_distinct},
    {"key를 수천 개 저장한 뒤에도 모든 key를 조회할 수 있다", all_keys_found_after_storing_thousands},
    {"store_destroy 뒤에 AddressSanitizer가 누수를 보고하지 않는다", destroy_leaves_no_leak},
    {"[T3] 같은 key에 두 번 저장하면 나중 value를 돌려준다", set_twice_returns_later_value},
    {"길이가 다른 value로 덮어써도 새 길이를 돌려준다", overwrite_with_different_length_returns_new_length},
    {"덮어쓴 뒤 AddressSanitizer가 옛 entry의 누수를 보고하지 않는다", overwrite_leaves_no_leak_of_old_entry},
    {"[T4] 있는 key를 삭제하면 지웠다고 알리고, 그 뒤 조회되지 않는다", delete_existing_key_reports_removed_and_get_fails},
    {NULL, NULL},
};
