# 07: README와 요구사항 표에 M3의 결과를 적는다

**Status:** ready-for-agent

**What to build:** client 개발자가 `README.md`만 보고 `408`, `503`, 연결 상한, 쉬는 시간 기한을 알 수 있습니다. 팀원은 `docs/requirements.md`의 L8 줄에서 우리 해석과 충족 여부, 근거를 봅니다. 결정은 `../spec.md`의 Solution, 응답 표, Further Notes를 따릅니다.

**Blocked by:** 03, 04, 05, 06

완료 조건:

- [ ] `docs/requirements.md` L8의 `해석` 칸: 동시성 문제는 요청 하나 때문에 다른 요청의 응답이 늦어지는 지연이고, 서버는 프로세스 1개, 스레드 1개로 결과가 틀리는 문제를 막는다
- [ ] L8의 `충족` 칸은 06의 판정에 따라 `충족` 또는 `부분`으로 적는다
- [ ] L8의 `근거` 칸에 `T12`, `T13`, `T14`, `single-thread-event-loop`를 적는다
- [ ] L8의 `남은 일` 칸에 06에서 고치지 않은 초과 항목과, 바이트를 아주 조금씩 계속 주고받는 연결을 적는다
- [ ] `README.md`의 "API" 절: 응답 표와 오류 이유 목록에 `408 request_timeout`, `503 too_many_connections`를 더하고, 연결 상한 128과 쉬는 시간 기한 10초를 적는다
- [ ] `README.md`의 "알려진 한계": 끝까지 보내지 않는 연결의 줄을 지우고, 바이트를 아주 조금씩 계속 주고받는 연결과 06에서 남은 초과 항목을 더한다
- [ ] `README.md`가 `../spec.md`의 `## Redis와 대조` 절을 링크한다(CONVENTION의 "마일스톤")
- [ ] `make verify`가 통과한다(`scripts/check-ids.py`가 인용한 ID와 ADR 이름을 확인한다)
