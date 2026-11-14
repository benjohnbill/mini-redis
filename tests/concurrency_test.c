#include <string.h>

#include "acutest.h"
#include "api.h"

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

static void request_timeout_returns_408_request_timeout(void)
{
    struct api_response response = api_reject_timeout();

    expect_error(response, 408, "{\"error\":\"request_timeout\"}");
}

static void too_many_connections_returns_503_json(void)
{
    struct api_response response = api_reject_connection();

    expect_error(response, 503, "{\"error\":\"too_many_connections\"}");
}

TEST_LIST = {
    {"요청 시간 초과 응답은 408과 {\"error\":\"request_timeout\"}이고 "
     "Content-Type은 application/json이다",
     request_timeout_returns_408_request_timeout},
    {"연결 수 초과 응답은 503과 {\"error\":\"too_many_connections\"}이고 "
     "Content-Type은 application/json이다",
     too_many_connections_returns_503_json},
    {NULL, NULL},
};
