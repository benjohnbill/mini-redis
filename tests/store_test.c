#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "acutest.h"
#include "store.h"

/* 만료와 상관없는 테스트가 넘기는 "지금"입니다. 모든 key를 만료 없이 저장하므로
   어떤 시각이어도 결과가 같습니다. */
#define ANY_NOW 0

/* key를 조회해 찾았는지, 길이, 바이트를 차례로 확인합니다. 찾지 못하면 value가
   NULL일 수 있으므로 memcmp 전에 멈춥니다. 그러지 않으면 UBSan이 자식 프로세스를
   멈춰 acutest가 실패 메시지를 잃습니다. */
static void check_get(struct store *store, const void *key, size_t key_len,
                      const void *want, size_t want_len)
{
    const void *value = NULL;
    size_t value_len = SIZE_MAX;
    if (!TEST_CHECK(
            store_get(store, ANY_NOW, key, key_len, &value, &value_len))) {
        TEST_MSG("못 찾은 key: \"%.*s\" (길이 %zu)", (int)key_len,
                 key_len > 0 ? (const char *)key : "", key_len);
        return;
    }
    if (!TEST_CHECK(value_len == want_len)) {
        TEST_MSG("value 길이: 기대 %zu, 실제 %zu", want_len, value_len);
        return;
    }
    TEST_CHECK(memcmp(value, want, want_len) == 0);
}

static void set_then_get_returns_value_and_length(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    TEST_CHECK(store_set(store, "name", 4, "jungle", 6, STORE_NO_EXPIRY));

    check_get(store, "name", 4, "jungle", 6);

    store_destroy(store);
}

static void get_missing_key_reports_not_found(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_CHECK(store_set(store, "name", 4, "jungle", 6, STORE_NO_EXPIRY));

    const void *value = NULL;
    size_t value_len = 0;
    TEST_CHECK(!store_get(store, ANY_NOW, "age", 3, &value, &value_len));

    store_destroy(store);
}

static void empty_value_is_found_with_length_zero(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_CHECK(store_set(store, "empty", 5, NULL, 0, STORE_NO_EXPIRY));

    check_get(store, "empty", 5, "", 0);

    store_destroy(store);
}

static void empty_key_can_be_set_and_get(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_CHECK(store_set(store, NULL, 0, "v", 1, STORE_NO_EXPIRY));

    check_get(store, NULL, 0, "v", 1);

    store_destroy(store);
}

static void value_with_inner_nul_is_not_truncated(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    const char bin[5] = {'a', '\0', 'b', '\0', 'c'};
    TEST_CHECK(store_set(store, "bin", 3, bin, 5, STORE_NO_EXPIRY));

    check_get(store, "bin", 3, "a\0b\0c", 5);

    store_destroy(store);
}

static void changing_caller_buffer_after_set_keeps_stored_value(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    char key[] = "name";
    char value_text[] = "jungle";
    TEST_CHECK(store_set(store, key, 4, value_text, 6, STORE_NO_EXPIRY));
    memcpy(value_text, "XXXXXX", 6);

    check_get(store, "name", 4, "jungle", 6);

    /* 호출한 쪽 key 버퍼를 바꿔도 저장된 key로 찾을 수 있어야 합니다. */
    memcpy(key, "XXXX", 4);
    check_get(store, "name", 4, "jungle", 6);

    store_destroy(store);
}

static void keys_sharing_prefix_with_different_length_are_distinct(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_CHECK(store_set(store, "abc", 3, "short", 5, STORE_NO_EXPIRY));
    TEST_CHECK(store_set(store, "abcd", 4, "long", 4, STORE_NO_EXPIRY));

    check_get(store, "abc", 3, "short", 5);
    check_get(store, "abcd", 4, "long", 4);

    /* 두 key가 다른 버킷에 가면 비교가 일어나지 않습니다. "a", "aa", ... 64개는
       서로 앞부분이 같고, 버킷 수보다 많아서 같은 버킷에서 비교되는 쌍이 생깁니다.
       value에는 key의 길이를 1바이트로 적습니다. */
    const void *value = NULL;
    size_t value_len = 0;
    char key[64];
    memset(key, 'a', sizeof key);
    for (size_t len = 1; len <= sizeof key; len++) {
        unsigned char len_as_byte = (unsigned char)len;
        TEST_ASSERT(
            store_set(store, key, len, &len_as_byte, 1, STORE_NO_EXPIRY));
    }
    for (size_t len = 1; len <= sizeof key; len++) {
        if (!TEST_CHECK(
                store_get(store, ANY_NOW, key, len, &value, &value_len))) {
            TEST_MSG("못 찾은 key 길이: %zu", len);
            break;
        }
        TEST_CHECK(value_len == 1);
        TEST_CHECK(*(const unsigned char *)value == len);
    }

    store_destroy(store);
}

/* 수천 개면 hash.c의 rehash가 여러 번 일어납니다. */
#define MANY_KEYS 5000

static void all_keys_found_after_storing_thousands(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    char key[16];
    char value_text[16];
    for (int i = 0; i < MANY_KEYS; i++) {
        int key_text_len = snprintf(key, sizeof key, "k%d", i);
        int value_text_len = snprintf(value_text, sizeof value_text, "v%d", i);
        TEST_ASSERT(store_set(store, key, (size_t)key_text_len, value_text,
                              (size_t)value_text_len, STORE_NO_EXPIRY));
    }

    for (int i = 0; i < MANY_KEYS; i++) {
        int key_text_len = snprintf(key, sizeof key, "k%d", i);
        int value_text_len = snprintf(value_text, sizeof value_text, "v%d", i);
        const void *value = NULL;
        size_t value_len = 0;
        if (!TEST_CHECK(store_get(store, ANY_NOW, key, (size_t)key_text_len,
                                  &value, &value_len))) {
            TEST_MSG("못 찾은 key: %s", key);
            break;
        }
        TEST_CHECK(value_len == (size_t)value_text_len);
        TEST_CHECK(memcmp(value, value_text, (size_t)value_text_len) == 0);
    }

    store_destroy(store);
}

/* entry가 남은 채로 없애도 AddressSanitizer(LeakSanitizer)가 누수를 보고하지
   않아야 합니다. acutest는 테스트마다 자식 프로세스를 exit()로 끝내므로, 누수가
   있으면 이 테스트가 실패합니다. */
static void destroy_leaves_no_leak(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_CHECK(store_set(store, "a", 1, "1", 1, STORE_NO_EXPIRY));
    TEST_CHECK(store_set(store, NULL, 0, NULL, 0, STORE_NO_EXPIRY));
    TEST_CHECK(store_set(store, "b", 1, "22", 2, STORE_NO_EXPIRY));
    store_destroy(store);
}

static void set_twice_returns_later_value(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    TEST_CHECK(store_set(store, "name", 4, "jungle", 6, STORE_NO_EXPIRY));
    TEST_CHECK(store_set(store, "name", 4, "krafto", 6, STORE_NO_EXPIRY));

    check_get(store, "name", 4, "krafto", 6);

    store_destroy(store);
}

static void overwrite_with_different_length_returns_new_length(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);

    TEST_CHECK(store_set(store, "name", 4, "jungle", 6, STORE_NO_EXPIRY));
    TEST_CHECK(store_set(store, "name", 4, "ab", 2, STORE_NO_EXPIRY));

    check_get(store, "name", 4, "ab", 2);

    TEST_CHECK(
        store_set(store, "name", 4, "longer value", 12, STORE_NO_EXPIRY));
    check_get(store, "name", 4, "longer value", 12);

    store_destroy(store);
}

/* 덮어쓰면 밀려난 옛 entry를 store가 해제해야 합니다. 해제하지 않으면 테이블에서
   빠진 옛 entry를 가리키는 곳이 없어 store_destroy도 해제하지 못하고,
   LeakSanitizer가 자식 프로세스 종료 때 누수로 보고합니다. */
static void overwrite_leaves_no_leak_of_old_entry(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_CHECK(store_set(store, "name", 4, "jungle", 6, STORE_NO_EXPIRY));
    TEST_CHECK(store_set(store, "name", 4, "krafton", 7, STORE_NO_EXPIRY));
    TEST_CHECK(store_set(store, "name", 4, "x", 1, STORE_NO_EXPIRY));
    store_destroy(store);
}

static void delete_existing_key_reports_removed_and_get_fails(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_CHECK(store_set(store, "name", 4, "jungle", 6, STORE_NO_EXPIRY));

    TEST_CHECK(store_delete(store, ANY_NOW, "name", 4));

    const void *value = NULL;
    size_t value_len = 0;
    TEST_CHECK(!store_get(store, ANY_NOW, "name", 4, &value, &value_len));

    store_destroy(store);
}

static void delete_missing_key_reports_nothing_removed(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_CHECK(store_set(store, "name", 4, "jungle", 6, STORE_NO_EXPIRY));

    TEST_CHECK(!store_delete(store, ANY_NOW, "age", 3));

    store_destroy(store);
}

static void delete_same_key_twice_reports_nothing_removed_second_time(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_CHECK(store_set(store, "name", 4, "jungle", 6, STORE_NO_EXPIRY));

    TEST_CHECK(store_delete(store, ANY_NOW, "name", 4));
    TEST_CHECK(!store_delete(store, ANY_NOW, "name", 4));

    store_destroy(store);
}

static void delete_one_key_keeps_other_keys(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_CHECK(store_set(store, "name", 4, "jungle", 6, STORE_NO_EXPIRY));
    TEST_CHECK(store_set(store, "age", 3, "7", 1, STORE_NO_EXPIRY));

    TEST_CHECK(store_delete(store, ANY_NOW, "name", 4));

    check_get(store, "age", 3, "7", 1);

    store_destroy(store);
}

/* 덮어쓰기가 옛 entry를 테이블에 남기면, 삭제 한 번 뒤에도 옛 value가 조회되거나
   옛 entry가 누수로 보고됩니다. */
static void delete_once_after_setting_twice_makes_key_not_found(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_CHECK(store_set(store, "name", 4, "jungle", 6, STORE_NO_EXPIRY));
    TEST_CHECK(store_set(store, "name", 4, "krafto", 6, STORE_NO_EXPIRY));

    TEST_CHECK(store_delete(store, ANY_NOW, "name", 4));

    const void *value = NULL;
    size_t value_len = 0;
    TEST_CHECK(!store_get(store, ANY_NOW, "name", 4, &value, &value_len));

    store_destroy(store);
}

static void empty_key_can_be_deleted(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_CHECK(store_set(store, NULL, 0, "v", 1, STORE_NO_EXPIRY));

    TEST_CHECK(store_delete(store, ANY_NOW, NULL, 0));

    const void *value = NULL;
    size_t value_len = 0;
    TEST_CHECK(!store_get(store, ANY_NOW, NULL, 0, &value, &value_len));

    store_destroy(store);
}

/* 길이의 합이 size_t를 넘으면 malloc 크기가 작은 값으로 감겨, 복사가 할당한
   영역 밖을 씁니다. store는 바이트를 읽기 전에 실패해야 하므로 value 버퍼는
   1바이트로 충분합니다. */
static void set_with_overflowing_length_fails_and_keeps_store(void)
{
    struct store *store = store_create();
    TEST_ASSERT(store != NULL);
    TEST_CHECK(store_set(store, "name", 4, "jungle", 6, STORE_NO_EXPIRY));

    TEST_CHECK(!store_set(store, "name", 4, "x", SIZE_MAX, STORE_NO_EXPIRY));

    check_get(store, "name", 4, "jungle", 6);

    store_destroy(store);
}

TEST_LIST = {
    {"[T1] 저장한 key를 조회하면 저장한 value와 길이를 돌려준다",
     set_then_get_returns_value_and_length},
    {"[T2] 없는 key를 조회하면 찾지 못했다고 알린다",
     get_missing_key_reports_not_found},
    {"0바이트 value를 저장하면 찾았고 길이가 0이라고 알린다",
     empty_value_is_found_with_length_zero},
    {"0바이트 key도 저장하고 조회할 수 있다", empty_key_can_be_set_and_get},
    {"중간에 '\\0'이 있는 value를 잘리지 않고 돌려준다",
     value_with_inner_nul_is_not_truncated},
    {"저장한 뒤 호출한 쪽 버퍼를 바꿔도 저장된 value는 그대로다",
     changing_caller_buffer_after_set_keeps_stored_value},
    {"앞부분이 같고 길이가 다른 key를 서로 다른 key로 다룬다",
     keys_sharing_prefix_with_different_length_are_distinct},
    {"key를 수천 개 저장한 뒤에도 모든 key를 조회할 수 있다",
     all_keys_found_after_storing_thousands},
    {"store_destroy 뒤에 AddressSanitizer가 누수를 보고하지 않는다",
     destroy_leaves_no_leak},
    {"[T3] 같은 key에 두 번 저장하면 나중 value를 돌려준다",
     set_twice_returns_later_value},
    {"길이가 다른 value로 덮어써도 새 길이를 돌려준다",
     overwrite_with_different_length_returns_new_length},
    {"덮어쓴 뒤 AddressSanitizer가 옛 entry의 누수를 보고하지 않는다",
     overwrite_leaves_no_leak_of_old_entry},
    {"[T4] 있는 key를 삭제하면 지웠다고 알리고, 그 뒤 조회되지 않는다",
     delete_existing_key_reports_removed_and_get_fails},
    {"없는 key를 삭제하면 지운 것이 없다고 알린다",
     delete_missing_key_reports_nothing_removed},
    {"같은 key를 두 번 삭제하면 두 번째는 지운 것이 없다고 알린다",
     delete_same_key_twice_reports_nothing_removed_second_time},
    {"한 key를 삭제해도 다른 key는 그대로 조회된다",
     delete_one_key_keeps_other_keys},
    {"같은 key에 두 번 저장한 뒤 한 번 삭제하면 조회되지 않는다",
     delete_once_after_setting_twice_makes_key_not_found},
    {"0바이트 key도 삭제할 수 있다", empty_key_can_be_deleted},
    {"key와 value 길이의 합이 너무 크면 저장에 실패하고 store는 그대로다",
     set_with_overflowing_length_fails_and_keeps_store},
    {NULL, NULL},
};
