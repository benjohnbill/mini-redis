---
seq: 1
topic: lab-baseline
date: 2026-11-14
who: DA
commit: f662f6b
previous: none
machine: "Intel Core Ultra 9 275HX(논리 코어 24개), Windows 11 Pro의 WSL2 Ubuntu 26.04(커널 6.18.33.2-microsoft-standard-WSL2, 메모리 15 GiB), gcc 15.2.0, wrk 4.1.0, Python 3.14.4"
build: "make bench: -std=c11 -Wall -Wextra -Werror -O2, sanitizer 끔, assert 남김, mongoose에 -D_DEFAULT_SOURCE -DMG_MAX_RECV_SIZE=16842752"
command: "make bench (시나리오 lab, prefill)"
milestones: [concurrency]
data: none
---

# 001 실험실 기준선과 미리 채우기

## 앞 기록과 달라진 점

첫 기록입니다. 서버는 쉬는 시간 기한(M3 티켓 03)과 연결 수 상한(티켓 04)을 합친 코드입니다. 처음 숫자(`65eb26f`)는 합치기 전 서버로 쟀고, 이 기록은 합친 서버로 다시 잰 숫자입니다. 이 기록은 `docs/milestones/concurrency/spec.md`의 `## 측정 결과`에서 옮겼습니다. `commit`은 결과를 처음 적은 커밋입니다.

## 결론

- 방해가 없을 때 작은 GET의 p99는 0.45\~0.49 ms로, 목표([M3 spec](../../milestones/concurrency/spec.md) Q10, 10 ms)의 약 1/20입니다.
- key 4,194,304개를 14.8초에 저장했습니다(초당 약 283,000개).

## 실험실 기준선 (티켓 01)

key 100,000개(value 100바이트)를 미리 채운 뒤, 그 key 가운데 무작위 하나를 고르는 작은 GET만 100연결로 10초 동안 보냈습니다. 지연은 wrk가 잰 값이고, 단위는 ms입니다. 서버는 쉬는 시간 기한(03)과 연결 수 상한(04)을 합친 코드입니다.

```console
$ wrk -t4 -c100 -d600s -s scripts/bench/prefill.lua <url> -- 100000 4 100 <완료 표시 디렉터리>
$ wrk --latency -t4 -c100 -d10s -s scripts/bench/lab_get.lua <url> -- 100000
```

| 회 | p50 | p99 | 초당 처리량 |
|---|---|---|---|
| 1 | 0.285 | 0.488 | 318,702 |
| 2 | 0.284 | 0.476 | 321,091 |
| 3 | 0.276 | 0.453 | 329,079 |

방해가 없을 때 작은 GET의 p99는 목표(Q10, 10 ms)의 약 1/20입니다. [M3 spec](../../milestones/concurrency/spec.md)의 "Redis와 대조"에 적은 방해 없음(p50 0.247, p99 0.975)과 도구가 달라(`redis-benchmark`와 wrk) 나란히 놓고 볼 때만 참고합니다.

## 미리 채우기 (티켓 01)

티켓 01의 `prefill` 시나리오가 key 4,194,304개(2²²)를 14.8초에 저장했습니다(초당 약 283,000개). 티켓 02부터는 `c-rehash` 시나리오가 같은 미리 채우기로 시작하므로 `prefill` 시나리오는 따로 두지 않습니다. 채운 뒤 처음, 가운데, 끝과 무작위 key 8개를 조회해 value가 맞는지 보고, 범위 밖 key `prefill-4194304`는 없음(`404`)을 확인합니다. key 100,003개로 미리 채운 뒤 모든 key를 하나씩 조회해, 빠진 key와 범위 밖 key가 없음도 한 번 확인했습니다.

```console
$ wrk -t4 -c100 -d600s -s scripts/bench/prefill.lua <url> -- 4194304 4 100 <완료 표시 디렉터리>
```

이 저장 동안 wrk가 잰 최대 지연은 328.46 ms였습니다. 저장만 보내는 닫힌 루프의 값이라, 작은 GET의 지연은 열린 루프 시나리오(티켓 02)가 잽니다.

- wrk는 시간으로 끝나므로, 스레드마다 맡은 저장의 응답을 모두 받으면 그 스레드를 멈추고 표시 파일을 남깁니다. `bench.py`는 표시 파일이 스레드 수만큼 생기면 wrk에 SIGINT를 보냅니다.
- 저장의 응답(`204`)에는 헤더가 없어 wrk가 Lua의 `response()`를 부르지 않습니다. 그래서 받은 응답의 수를 `request()`가 불린 횟수에서 연결 수를 빼서 셉니다. wrk 4.1.0은 시작 전에 첫 스레드의 `request()`를 한 번 더 부르므로 그 호출은 세지 않습니다. 자세한 내용은 `scripts/bench/prefill.lua`의 주석에 있습니다.
- wrk는 연결 하나로 요청을 이어 보내므로, 측정은 서버가 같은 연결의 두 번째 요청에도 답한다는 것(기능 테스트 "(기능) 연결 하나로 보낸 두 번째 요청도 응답을 받는다")에 기댑니다.
