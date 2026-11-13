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

TEST_LIST = {
    {"[T1] 저장한 key를 조회하면 저장한 value와 길이를 돌려준다", set_then_get_returns_value_and_length},
    {NULL, NULL},
};
