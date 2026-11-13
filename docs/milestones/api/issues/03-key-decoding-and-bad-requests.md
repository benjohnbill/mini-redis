# 03: key 해석과 잘못된 경로, 메서드

**Status:** ready-for-agent

**What to build:** 다른 서버가 `/`, 공백, 한글, `'\0'`이 든 key를 퍼센트 인코딩으로 보내면, 서버는 `%HH`를 정확히 한 번 풀어 그 바이트 그대로 key로 씁니다. 빈 key, 깨진 인코딩, `/keys/`로 시작하지 않는 경로, 지원하지 않는 메서드는 각각의 오류 이유로 알립니다. api 층은 경로, 메서드, key 순서로 검사하고 처음 걸린 오류 하나로 답합니다. store의 0바이트 key 지원은 고치지 않습니다. 결정은 `../spec.md`의 Q2, Q3, "오류의 우선순위"를 따릅니다.

**Blocked by:** 02

테스트 시나리오(팀 확인 2026-11-13). 테스트 이름은 아래 문장을 그대로 씁니다.

- [x] %2F는 경로 구분자가 아니라 key 안의 / 바이트가 된다
- [x] %00이 든 key를 잘리지 않고 저장하고 조회한다
- [x] %20이 든 key를 공백 바이트 그대로 key로 쓴다
- [x] %2541은 한 번만 풀려 %41이라는 key가 된다
- [x] 한글을 인코딩한 key를 그 UTF-8 바이트 그대로 key로 쓴다
- [x] /keys/처럼 key가 비면 400과 {"error":"empty_key"}를 돌려준다
- [x] %G1이나 끝이 잘린 %2는 400과 {"error":"bad_key_encoding"}을 돌려준다
- [x] /keys/로 시작하지 않는 경로는 404와 {"error":"route_not_found"}를 돌려준다
- [x] /keys/<key>에 POST를 보내면 405와 {"error":"method_not_allowed"}를 돌려준다
- [x] 경로와 메서드가 모두 틀리면 경로 오류인 404와 route_not_found를 돌려준다
- [x] 메서드가 틀리고 key가 비면 메서드 오류인 405를 돌려준다
- [x] 끝에 /가 없는 /keys는 404와 route_not_found를 돌려준다
- [x] (기능) %00이 든 key가 실제 서버를 지나도 잘리지 않는다
- [x] `make verify`가 통과한다

## Comments

- `api_handle`의 시그니처는 그대로입니다. 안에서 `path_is_keys`(404), `key_action_parse`(405), `key_request_handle`(key를 `malloc`한 버퍼에 한 번 풀고 400을 검사) 순서로 봅니다. 413(04)은 body가 오기 전의 헤더 단계이므로 따로 더하는 공개 함수가 맡고, 오류 body는 같은 `response_error(413, ERROR_BODY("body_too_large"))`로 만들면 됩니다.
- 임시 응답 `501`과 `response_unhandled`는 없어졌습니다. 서버의 `status_reason`에 `400`, `405`를 더하고 `501`을 뺐으니, 04는 `413`의 문구를 더합니다. `405` 응답의 `Allow` 헤더는 spec에 없어서 넣지 않았습니다.
