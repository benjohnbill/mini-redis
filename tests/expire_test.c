#include <stdint.h>
#include <string.h>

#include "acutest.h"
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

TEST_LIST = {
    {"만료 없이 저장한 key는 아주 먼 시각에 조회해도 value를 돌려준다",
     key_without_expiry_is_found_at_far_future},
    {NULL, NULL},
};
