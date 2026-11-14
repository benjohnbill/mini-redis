#ifndef API_H
#define API_H

/* api 층: 요청의 메서드, 경로, body를 받아 store를 부르고 응답을 정합니다.
   연결과 HTTP 라이브러리를 모르므로, 모든 입력과 출력은 주소와 길이 한 쌍입니다.
   경로는 요청 줄의 대상 전부를 퍼센트 인코딩을 풀지 않은 그대로 받습니다.
   ? 뒤의 query도 잘라내지 않고 넘겨야, api 층이 key 안의 ?를 거절할 수
   있습니다. */

#include <stdbool.h>
#include <stddef.h>

#include "store.h"

/* body의 상한 16 MiB입니다(spec Q10). 요청 하나의 메모리를 수신 버퍼와
   store의 복사본, 약 32 MiB로 묶습니다. */
#define API_BODY_MAX 16777216

struct api_response {
    int status;
    /* body가 없으면 NULL입니다. */
    const char *content_type;
    /* 조회 성공이면 store_get이 빌려준 value를 가리킵니다. 같은 store에
       다음 store 호출을 하기 전에 응답을 보내야 합니다. */
    const void *body;
    size_t body_len;
    /* 405일 때 Allow 헤더에 실을 메서드 목록이고, 그 밖에는 NULL입니다. */
    const char *allow;
};

/* 요청 하나를 처리하고 응답을 돌려줍니다. */
struct api_response api_handle(struct store *store, const char *method,
                               size_t method_len, const char *path,
                               size_t path_len, const void *body,
                               size_t body_len);

/* 헤더만 보고 거절할 요청이면 오류 응답을 *response에 쓰고 true를 돌려줍니다.
   Transfer-Encoding 헤더가 있으면 411, 헤더가 알린 body 길이가 API_BODY_MAX를
   넘으면 413이고, 이 순서로 검사합니다. 서버는 body를 받기 전에 이 응답을
   보내고 연결을 닫습니다. 거절하지 않으면 *response를 건드리지 않고 false를
   돌려줍니다. */
bool api_reject_headers(bool has_transfer_encoding, size_t body_len,
                        struct api_response *response);

/* 동시 연결이 상한에 차 있을 때 새 연결에 보낼 503 응답입니다(spec Q8).
   서버는 이 응답을 보낸 뒤 연결을 닫습니다. */
struct api_response api_too_many_connections(void);

#endif /* API_H */
