# 06: README와 요구사항 표에 M4 결과를 적는다

**Status:** ready-for-agent

**What to build:** client 개발자는 `README.md`의 "API" 절에서 `TTL` 헤더의 단위, 받는 값, 덮어쓰기 규칙, `400 bad_ttl` 줄을 읽습니다. 운영하는 팀원은 "알려진 한계"에서 M4의 한계를 읽고, 요구사항 표에서 L9의 충족과 근거를 봅니다.

**Blocked by:** 03, 05

그 밖의 완료 조건:

- [ ] `README.md`의 "API" 절과 응답 표, 오류 이유 목록에 `TTL`과 `bad_ttl`을 더한다
- [ ] "알려진 한계"에 절전 중 시계가 멈춤, 전송 지연만큼 늦은 만료, 그리고 측정에서 나온 한계를 적을지 정해 적는다
- [ ] `README.md`의 마일스톤 표에서 M4의 상태와 "Redis와 대조" 링크를 고친다
- [ ] `docs/requirements.md`의 L9에 해석, 충족, 근거(T15, T16, T17과 ADR 이름), 남은 일을 적는다
- [ ] `make verify`가 통과한다(ID 검사 포함)
