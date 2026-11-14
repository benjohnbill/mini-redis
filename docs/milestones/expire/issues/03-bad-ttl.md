# 03: 잘못된 TTL은 400 bad_ttl을 받는다

**Status:** ready-for-agent

**What to build:** `PUT`의 `TTL`이 1 이상의 정수가 아니거나, ms로 바꿔 지금 시각에 더했을 때 넘치거나, 한 요청에 두 번 오면, client는 `400`과 `{"error":"bad_ttl"}`을 받고 store는 바뀌지 않습니다(Q12, Q14, Q15). `GET`과 `DELETE`에 붙은 `TTL`은 무시합니다. 응답은 api 층이 만들고, 서버는 `TTL` 헤더의 개수를 세어 넘깁니다. 결정은 `../spec.md`의 응답 표를 따릅니다.

**Blocked by:** 02

테스트 시나리오(팀 확인). 테스트 이름은 아래 문장을 그대로 씁니다.

- [x] TTL이 0이면 400과 bad_ttl을 받고 저장하지 않는다
- [x] TTL이 음수이면 400과 bad_ttl을 받는다
- [x] TTL이 숫자가 아니면 400과 bad_ttl을 받는다
- [x] TTL이 빈 값이면 400과 bad_ttl을 받는다
- [x] TTL에 숫자 뒤로 다른 문자가 붙으면 400과 bad_ttl을 받는다
- [x] 지금 시각에 더하면 넘치는 TTL은 400과 bad_ttl을 받는다
- [x] TTL 헤더가 두 번 오면 400과 bad_ttl을 받는다
- [x] bad_ttl 응답의 Content-Type은 application/json이다
- [x] GET에 붙은 TTL은 무시하고 value를 돌려준다
- [x] (기능) TTL 헤더를 두 번 보낸 PUT은 400과 bad_ttl을 받는다

그 밖의 완료 조건:

- [x] 앞뒤 공백과 `+` 부호를 받을지 정하고, 정한 것을 테스트 이름과 `../spec.md`에 남긴다
- [x] `make verify`가 통과한다
