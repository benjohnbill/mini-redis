---
seq: 2
topic: sleep-until-due
date: 2026-11-18
who: CJG
commit: ef385a0
previous: 001-CJG-baseline.md
machine: "Intel Core Ultra 7 155H(논리 코어 22개), Windows의 WSL2 Ubuntu 26.04.1(커널 6.18.58.1-microsoft-standard-WSL2, 메모리 15 GiB), gcc 15.2.0, wrk 4.1.0, Python 3.14.4"
build: "make -B build/bench/mini-redis: -O2, sanitizer 끔, assert 남김, SWEEP_TRACE 켬"
command: "python3 -B scripts/bench/idle_wakeup.py build/bench/mini-redis (상태마다 5초씩 3회)"
milestones: [perf]
data: none
---

# 002 다음에 할 일이 생길 때까지 잠드는 서버

## 앞 기록과 달라진 점

- **코드:** 루프가 25 ms씩 기다리지 않고, 매 바퀴 `poll_timeout_decide`가 정한 시간만큼 잡니다. 다음 주기 청소가 지울 key가 생기는 시각, 가장 이른 쉬는 연결 기한, 상한 `POLL_TIMEOUT_MAX_MS`(1초) 가운데 가장 이른 시각까지입니다(`0c361eb`). `ef385a0`은 측정 도구와 문서만 바꿨으므로 서버는 `0c361eb`와 같습니다.
- **측정 조건:** 001과 같은 머신, 같은 빌드, 같은 스크립트로 001 바로 뒤에 쟀습니다.

## 결론

- **세 상태 모두 초당 39.8번에서 1.0번으로 줄었습니다.** 남은 1번은 상한 1초 때문에 깨어나는 것입니다. 상한은 `stop_requested`를 확인한 뒤와 잠들기 전 사이에 온 신호를 놓치지 않기 위해 둡니다.
- **보관 기간 1시간인 key가 있어도 1.0번입니다.** 가장 이른 만료 시각이 상한보다 멀기 때문입니다. 만료가 이어지는 부하에서 청소 간격과 작은 GET의 지연이 그대로인지는 [server/007](../server/007-CJG-poll-timeout.md)에서 봅니다.
- **쉬는 연결이 있어도 1.0번입니다.** 연결의 쉬는 시간 기한(10초)이 상한보다 멀기 때문입니다. 기한에 맞춰 닫는지는 `tests/functional/concurrency_test.py`의 10초 기다림 테스트가 확인합니다.

## 결과

측정 방법은 001과 같습니다.

| 상태 | 1회 | 2회 | 3회 |
|---|---|---|---|
| `empty`: key와 연결이 없음 | 1.0 | 1.0 | 1.0 |
| `ttl-key`: 보관 기간 1시간인 key 1개 | 1.0 | 1.0 | 1.0 |
| `idle-conn`: 아무것도 보내지 않는 연결 1개 | 1.0 | 1.0 | 1.0 |
