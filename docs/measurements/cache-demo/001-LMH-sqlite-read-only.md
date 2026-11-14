---
seq: 1
topic: sqlite-read-only
date: 2026-11-14
who: LMH
commit: 82b93ad
previous: none
machine: "macOS 26.6.2 arm64, python 3.15.2, SQLite 3.54.1"
build: "build/bench/cache-demo, build/bench/mini-redis: -O2, sanitizer 끔"
command: "python3 -B scripts/bench/cache_bench.py build/bench/cache-demo build/bench/mini-redis --warmup 3 --measure 10 --runs 2 --ttl 60 --scenario ..."
milestones: [cache]
data: none
---

# 001 SQLite 원본의 조회 전용 캐시 효과

## 앞 기록과 달라진 점

첫 기록입니다. 이 기록은 `docs/milestones/cache/spec.md`의 `## 측정 결과`에서 옮겼습니다. 결과는 처음에 `README.md`에 적혔고(`82b93ad`), `f1bfa6d`에서 spec으로 옮겨졌습니다. `commit`은 결과를 처음 적은 커밋입니다.

## 결론

- 같은 조건을 반복하는 목록·검색은 적중률 100%로 원본 조회가 사라지고 p50이 줄었습니다(10.2→2.7 ms, 3.3→2.9 ms).
- 상세 조회와 매번 조건이 달라지는 목록·검색은 단일 스레드 데모가 mini-redis 왕복을 기다리므로 캐시를 켜면 더 느렸습니다.

## 결과

2026-11-14, macOS 26.6.2 arm64, python 3.15.2, SQLite 3.54.1에서 `-O2`(sanitizer 끔) 빌드로 쟀습니다. 입력은 상품 10,000개 seed 1, 연결 32개, 초당 500요청, 준비 3초, 측정 10초, 2회, TTL 60초이고 `make cache-bench`의 기본값(준비 10초·30초·3회)보다 짧게 잰 값입니다. 명령은 `python3 -B scripts/bench/cache_bench.py build/bench/cache-demo build/bench/mini-redis --warmup 3 --measure 10 --runs 2 --ttl 60 --scenario ...`입니다. 처리량은 두 모드 모두 502.4/s(제안 부하 한도), 오류와 timeout은 0입니다.

| 시나리오 | cache off p50/p95 (ms) | cache on p50/p95 (ms) | 원본 조회 off→on | 적중률 |
|---|---|---|---|---|
| 상세, 균등 | 1.31 / 3.10 | 4.03 / 5.37 | 5,024→3,220 | 35.9% |
| 상세, 20%에 80% 집중 | 1.18 / 3.12 | 3.64 / 5.28 | 5,024→2,356 | 53.1% |
| 목록, 같은 조건 반복 | 10.20 / 14.51 | 2.67 / 3.36 | 5,024→0 | 100% |
| 목록, 다양한 조건 | 13.62 / 20.02 | 16.24 / 20.54 | 5,024→5,024 | 0% |
| 검색, 같은 조건 반복 | 3.32 / 5.20 | 2.91 / 3.48 | 5,024→0 | 100% |
| 검색, 다양한 조건 | 15.60 / 20.71 | 17.96 / 24.95 | 5,024→5,024 | 0% |

같은 조건을 반복하는 목록·검색은 적중률 100%로 원본 조회가 사라지고 p50이 10.2→2.7 ms, 3.3→2.9 ms로 줄었습니다. 반면 단일 스레드 데모가 mini-redis 왕복을 기다리므로 상세 조회는 p50이 1.2→ 3.6\~4.0 ms로 늘었고, 매번 조건이 달라지는 목록·검색은 적중이 없어 왕복 비용만 더해졌습니다. 이 수치는 합격 기준이 아니며 장비·부하·데이터가 다르면 달라집니다.
