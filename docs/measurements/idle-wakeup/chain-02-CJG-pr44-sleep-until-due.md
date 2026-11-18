---
seq: TBD
topic: chain-pr44-sleep-until-due
date: 2026-11-18
who: CJG
commit: 60d1e41
previous: chain-01-CJG-before-pr44.md
machine: "Intel Core Ultra 7 155H(논리 코어 22개), Windows의 WSL2 Ubuntu 26.04.1(커널 6.18.58.1-microsoft-standard-WSL2, 메모리 15 GiB), gcc 15.2.0, wrk 4.1.0, Python 3.14.4"
build: "make -B build/bench/mini-redis: -O2, sanitizer 끔, assert 남김, SWEEP_TRACE 켬"
command: "python3 -B scripts/bench/idle_wakeup.py build/bench/mini-redis (상태마다 5초씩 3회)"
milestones: [perf]
data: none
---

# 병합 사슬 #44: 다음 청소나 쉬는 연결 기한까지 잠듦

## 앞 기록과 달라진 점

- **코드:** 앞 기록(`66d6de7`)에 PR #44만 병합했습니다. 루프가 25 ms마다 깨어나지 않고, 다음 청소 기한이나 쉬는 연결의 기한까지(최대 `POLL_TIMEOUT_MAX_MS` 1초) 잠듭니다.

## 결론

- **세 상태 모두 초당 39.8번에서 1.0번으로 줄었습니다.** #44 자신의 기록([002](002-CJG-sleep-until-due.md))과 같은 결과입니다. 요청이 있을 때의 지연은 같은 commit의 서버 사슬 기록이 맡습니다.

## 결과

| 상태 | 1회 | 2회 | 3회 |
|---|---|---|---|
| `empty`: key와 연결이 없음 | 1.0 | 1.0 | 1.0 |
| `ttl-key`: 보관 기간 1시간인 key 1개 | 1.0 | 1.0 | 1.0 |
| `idle-conn`: 아무것도 보내지 않는 연결 1개 | 1.0 | 1.0 | 1.0 |
