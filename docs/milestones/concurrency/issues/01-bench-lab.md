# 01: `make bench`와 wrk 실험실로 방해 없는 기준선을 잰다

**Status:** ready-for-agent

**What to build:** 측정하는 팀원이 `make bench` 하나로 sanitizer를 끈 최적화 빌드의 서버를 띄우고, wrk로 방해가 없을 때 작은 GET 100연결의 p50, p99, 초당 처리량을 봅니다. 02가 쓸 미리 채우기(store에 4,194,304개를 빠르게 넣기)도 이 티켓이 만듭니다. 측정은 `make verify`와 CI에 넣지 않습니다. 결정은 `../spec.md`의 Q2, Q3, Q9를 따릅니다.

**Blocked by:** None (can start immediately)

완료 조건:

- [x] `make bench`가 `make verify`의 ASan 빌드와 섞이지 않는 별도 빌드(sanitizer 끔, 최적화)로 서버를 만들고, 띄우고, 측정이 끝나면 끈다
- [x] wrk 실험실: 작은 value(약 100바이트)의 key를 채운 뒤, 작은 GET만 100연결로 보내 p50, p99, 초당 처리량을 출력한다
- [x] wrk의 Lua 스크립트로 요청마다 key를 바꿔 4,194,304개(2²²)를 저장하는 미리 채우기를 만든다. 채운 뒤 표본 key 몇 개를 조회해 들어갔는지 확인한다
- [x] wrk가 없으면 설치 명령(`sudo apt install wrk`)을 알려 주고 실패로 끝난다
- [x] 측정 스크립트는 `tests/` 밖에 둔다. `scripts/check-ids.py`가 `tests/` 아래의 모든 파일을 읽기 때문이다
- [x] `make verify`와 CI 설정은 바뀌지 않고, `make verify`가 통과한다
- [x] 기준선 숫자와 환경(머신, 빌드, 명령)을 `../spec.md`의 "측정 결과" 절에 적는다

## Comments

- 기준선(key 100,000개, 작은 GET 100연결, 10초, 3회): p50 0.275~0.280 ms, p99 0.445~0.476 ms, 초당 323,158~332,778개. 환경과 명령은 `../spec.md`의 "측정 결과"에 있습니다.
- 미리 채우기: key 4,194,304개를 14.4초에 저장합니다(초당 약 290,000개). 이 동안 wrk가 잰 최대 지연은 343.77 ms입니다(저장만 보내는 닫힌 루프). 작은 GET에 주는 영향은 02가 잽니다.
- 02가 쓸 것: `scripts/bench/bench.py`의 `prefill(server, key_count)`가 key `prefill-0`~`prefill-<key_count - 1>`에 `VALUE`(100바이트)를 넣고 표본을 확인합니다. 새 시나리오는 `SCENARIOS`에 함수 하나를 더하면 `make bench`가 함께 돌립니다.
- DA를 합치기 전에는 서버가 같은 연결의 두 번째 요청에 답하지 않아(mongoose의 `is_resp`를 내리는 곳이 없었음) wrk가 연결마다 첫 응답 뒤에 멈췄습니다. 같은 고침이 다른 티켓으로 먼저 DA에 들어와, 이 브랜치에서 따로 만든 고침과 테스트는 합칠 때 DA의 것으로 대신했습니다.
- wrk 4.1.0은 헤더가 없는 응답(우리 서버의 `204`)에 Lua `response()`를 부르지 않고, 시작 전에 첫 스레드의 `request()`를 한 번 더 부릅니다. 미리 채우기는 둘 다 감안해 응답 수를 셉니다(`scripts/bench/prefill.lua`).
