#ifndef API_H
#define API_H

/* api 층: 요청의 메서드, 경로, body를 받아 store를 부르고 응답을 정합니다.
   연결과 HTTP 라이브러리를 모르므로, 모든 입력과 출력은 주소와 길이 한 쌍입니다.
   경로는 요청 줄의 대상 전부를 퍼센트 인코딩을 풀지 않은 그대로 받습니다.
   ? 뒤의 query도 잘라내지 않고 넘겨야, api 층이 key 안의 ?를 거절할 수
   있습니다. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "store.h"

/* body의 상한 16 MiB입니다(spec Q10). 요청 하나의 메모리를 수신 버퍼와
   store의 복사본, 약 32 MiB로 묶습니다. */
#define API_BODY_MAX 16777216
/* POST /mget 한 번에 받는 key의 상한입니다. */
#define API_MGET_KEYS_MAX 64
/* POST /mget 응답에 담는 value 바이트 합계의 상한입니다. 서버는 스레드 하나라서
   거대한 응답 하나를 만드는 동안 다른 요청이 기다리므로, body와 같은 16 MiB로
   묶습니다. */
#define API_MGET_VALUES_MAX API_BODY_MAX

struct api_response {
    int status;
    /* body가 없으면 NULL입니다. */
    const char *content_type;
    /* 조회 성공이면 store_get이 빌려준 value를 가리킵니다. 같은 store에
       다음 store 호출을 하기 전에 응답을 보내야 합니다. POST /mget의 성공이면
       owned_body와 같은 주소입니다. */
    const void *body;
    size_t body_len;
    /* 405일 때 Allow 헤더에 실을 메서드 목록이고, 그 밖에는 NULL입니다. */
    const char *allow;
    /* api 층이 응답을 위해 할당한 메모리입니다. NULL이 아니면 응답을 보낸 뒤
       api_response_destroy로 해제해야 합니다. */
    void *owned_body;
};

/* 요청의 TTL 헤더입니다(ADR ttl-in-request-header). 서버는 헤더의 값을 해석하지
   않고 그대로 넘기며, 검사는 api 층이 합니다. */
struct api_ttl_header {
    /* 첫 번째 TTL 헤더 값의 바이트입니다. 헤더가 없으면 NULL이고 길이는 0입니다. */
    const char *text;
    size_t text_len;
    /* 요청에 온 TTL 헤더의 개수입니다. 0이면 보관 기간이 없는 PUT입니다. */
    size_t count;
    /* 요청 헤더를 다 받은 시각(mg_millis의 ms)입니다. 보관 기간은 이 시각부터
       셉니다(spec Q11). */
    uint64_t received_at;
};

/* now 시각(mg_millis의 ms)에 온 요청 하나를 처리하고 응답을 돌려줍니다.
   ttl_header는 PUT만 쓰고, GET과 DELETE는 무시합니다. NULL이면 안 됩니다.
   응답을 보낸 뒤에는 api_response_destroy를 불러야 합니다. */
struct api_response api_handle(struct store *store, uint64_t now,
                               const char *method, size_t method_len,
                               const char *path, size_t path_len,
                               const struct api_ttl_header *ttl_header,
                               const void *body, size_t body_len);

/* 응답이 가진 메모리(owned_body)를 해제하고 body를 비웁니다. 가진 메모리가
   없는 응답에 불러도 됩니다. response 자체는 부른 쪽의 값이라 해제하지
   않습니다. */
void api_response_destroy(struct api_response *response);

/* 헤더만 보고 거절할 요청이면 오류 응답을 *response에 쓰고 true를 돌려줍니다.
   헤더가 서버의 상한보다 많으면 431, Transfer-Encoding 헤더가 있으면 411, 헤더가
   알린 body 길이가 API_BODY_MAX를 넘으면 413이고, 이 순서로 검사합니다. 서버는
   body를 받기 전에 이 응답을 보내고 연결을 닫습니다. 거절하지 않으면 *response를
   건드리지 않고 false를 돌려줍니다. */
bool api_reject_headers(bool has_too_many_headers, bool has_transfer_encoding,
                        size_t body_len, struct api_response *response);

/* 요청을 받는 중에 너무 오래 멈춘 연결을 거절하는 408 응답을 돌려줍니다(spec
   Q4). 서버는 이 응답을 보낸 뒤 연결을 닫습니다. */
struct api_response api_reject_timeout(void);

/* 동시 연결이 상한에 차 있을 때 새 연결을 거절하는 503 응답을 돌려줍니다(spec
   Q8). 서버는 이 응답을 보낸 뒤 연결을 닫습니다. */
struct api_response api_reject_connection(void);

#endif /* API_H */
