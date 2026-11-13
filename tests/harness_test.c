#include "acutest.h"

static void empty_test(void)
{
}

TEST_LIST = {
    {"테스트 도구가 빈 테스트를 실행한다", empty_test},
    {NULL, NULL},
};
