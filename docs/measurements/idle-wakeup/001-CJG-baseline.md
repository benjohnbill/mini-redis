---
seq: 1
topic: baseline
date: 2026-11-18
who: CJG
commit: d64ae63
previous: none
machine: "Intel Core Ultra 7 155H(논리 코어 22개), Windows의 WSL2 Ubuntu 26.04.1(커널 6.18.58.1-microsoft-standard-WSL2, 메모리 15 GiB), gcc 15.2.0, wrk 4.1.0, Python 3.14.4"
build: "make -B build/bench/mini-redis: -O2, sanitizer 끔, assert 남김, SWEEP_TRACE 켬"
command: "python3 -B scripts/bench/idle_wakeup.py build/bench/mini-redis (ef385a0의 스크립트, 상태마다 5초씩 3회)"
milestones: [perf]
data: none
---

# 001 요청이 없는 서버의 깨어남 기준선

## 앞 기록과 달라진 점

- **목적:** `idle-wakeup/`의 첫 기록입니다. M4가 `POLL_INTERVAL_MS`를 25 ms로 정한 뒤로 요청이 없는 서버가 초당 40번 깨어난다고 계산했던 값([server/004](../server/004-CJG-expire-sweep.md))을 실제로 잽니다.
- **도구:** 스크립트는 이 코드보다 뒤에 만든 `ef385a0`의 `scripts/bench/idle_wakeup.py`입니다. 서버 코드는 `d64ae63`입니다.

## 결론

- **세 상태 모두 초당 39.8번 깨어납니다.** key가 있든 없든, 연결이 열려 있든 없든 같습니다. 서버가 할 일과 상관없이 `mg_mgr_poll`을 25 ms씩 기다리기 때문입니다.

## 결과

서버 프로세스의 문맥 전환 수(`/proc/<pid>/status`의 `voluntary_ctxt_switches`와 `nonvoluntary_ctxt_switches`의 합)가 5초 동안 늘어난 수를 초당으로 나눴습니다. 회마다 새 서버를 띄웁니다.

| 상태 | 1회 | 2회 | 3회 |
|---|---|---|---|
| `empty`: key와 연결이 없음 | 39.8 | 39.8 | 39.8 |
| `ttl-key`: 보관 기간 1시간인 key 1개 | 39.8 | 39.8 | 39.8 |
| `idle-conn`: 아무것도 보내지 않는 연결 1개 | 39.8 | 39.8 | 39.8 |
