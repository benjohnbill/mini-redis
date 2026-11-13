# 06: README의 "API" 절과 요구사항 기록

**Status:** ready-for-agent

**What to build:** 다른 서버의 개발자가 코드를 읽지 않고 `README.md`의 "API" 절만 보고 mini-redis를 쓸 수 있습니다(L10). 절에는 메서드와 경로, 응답 표, 오류 이유 목록, 저장, 조회, 삭제의 `curl` 예시를 담습니다. `docs/requirements.md`의 L3, L10, L14에 `근거`와 `충족`을 적고, 그 칸이 인용하는 테스트에 `T5`부터 ID를 붙입니다. 결정은 `../spec.md`의 Q13과 Testing Decisions의 "ID"를 따릅니다.

**Blocked by:** 05

- [ ] "API" 절의 응답 표가 `../spec.md`의 응답 표와 오류의 우선순위와 같다
- [ ] "API" 절의 `curl` 예시를 실제 서버에 그대로 보내면 적힌 응답이 온다
- [ ] L3, L10, L14의 `근거` 칸이 인용한 테스트 ID가 실제로 있다
- [ ] `make verify`가 통과한다

## Comments
