# 01: `make bench`와 wrk 실험실로 방해 없는 기준선을 잰다

**Status:** ready-for-agent

**What to build:** 측정하는 팀원이 `make bench` 하나로 sanitizer를 끈 최적화 빌드의 서버를 띄우고, wrk로 방해가 없을 때 작은 GET 100연결의 p50, p99, 초당 처리량을 봅니다. 02가 쓸 미리 채우기(store에 4,194,304개를 빠르게 넣기)도 이 티켓이 만듭니다. 측정은 `make verify`와 CI에 넣지 않습니다. 결정은 `../spec.md`의 Q2, Q3, Q9를 따릅니다.

**Blocked by:** None (can start immediately)

완료 조건:

- [ ] `make bench`가 `make verify`의 ASan 빌드와 섞이지 않는 별도 빌드(sanitizer 끔, 최적화)로 서버를 만들고, 띄우고, 측정이 끝나면 끈다
- [ ] wrk 실험실: 작은 value(약 100바이트)의 key를 채운 뒤, 작은 GET만 100연결로 보내 p50, p99, 초당 처리량을 출력한다
- [ ] wrk의 Lua 스크립트로 요청마다 key를 바꿔 4,194,304개(2²²)를 저장하는 미리 채우기를 만든다. 채운 뒤 표본 key 몇 개를 조회해 들어갔는지 확인한다
- [ ] wrk가 없으면 설치 명령(`sudo apt install wrk`)을 알려 주고 실패로 끝난다
- [ ] 측정 스크립트는 `tests/` 밖에 둔다. `scripts/check-ids.py`가 `tests/` 아래의 모든 파일을 읽기 때문이다
- [ ] `make verify`와 CI 설정은 바뀌지 않고, `make verify`가 통과한다
- [ ] 기준선 숫자와 환경(머신, 빌드, 명령)을 `../spec.md`의 "측정 결과" 절에 적는다
