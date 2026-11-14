#include <string.h>

#include "acutest.h"
#include "api.h"

static void request_timeout_returns_408_request_timeout(void)
{
    const char *body = "{\"error\":\"request_timeout\"}";

    struct api_response response = api_request_timeout();

    TEST_CHECK(response.status == 408);
    TEST_MSG("상태 코드: %d", response.status);
    TEST_ASSERT(response.content_type != NULL);
    TEST_CHECK(strcmp(response.content_type, "application/json") == 0);
    TEST_MSG("Content-Type: %s", response.content_type);
    TEST_ASSERT(response.body_len == strlen(body));
    TEST_CHECK(memcmp(response.body, body, response.body_len) == 0);
}

TEST_LIST = {
    {"요청 시간 초과 응답은 408과 {\"error\":\"request_timeout\"}이고 "
     "Content-Type은 application/json이다",
     request_timeout_returns_408_request_timeout},
    {NULL, NULL},
};
