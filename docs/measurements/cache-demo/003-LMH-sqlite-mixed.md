---
seq: 3
topic: sqlite-mixed
date: 2026-11-14
who: LMH
commit: 82b93ad
previous: 001-LMH-sqlite-read-only.md
machine: "원문에 적지 않았습니다. 같은 커밋의 001, 002와 같은 macOS 26.6.2 arm64, python 3.15.2, SQLite 3.54.1로 추정합니다"
build: "build/bench/cache-demo, build/bench/mini-redis: -O2, sanitizer 끔"
command: "python3 -B scripts/bench/cache_bench.py build/bench/cache-demo build/bench/mini-redis --warmup 3 --measure 10 --runs 1 --scenario ... (TTL 60초와 2초)"
milestones: [cache]
data: none
---

# 003 수정과 만료가 섞인 SQLite 캐시 효과

## 앞 기록과 달라진 점

`src/`는 001과 같습니다. `scripts/bench/cache_bench.py`에 조회·수정 비율 99:1과 90:10, TTL 2초를 더했고, 측정 뒤 cache off 데모와 응답을 대조하는 `correctness_check`를 더했습니다. 이 기록은 `docs/milestones/cache/issues/07-sqlite-mixed-benchmark.md`의 "구현과 검증"에서 옮겼습니다. `commit`은 결과를 처음 적은 커밋입니다.

## 결론

- 수정이 섞여도 상세 집중 조회는 원본 조회가 절반 이하로 줄었습니다.
- TTL 2초에서는 적중률이 53.1%→22.1%, 48.5%→20.7%로 떨어져 만료와 재저장의 비용이 드러났습니다.
- 목록·검색은 조건이 매번 달라 적중이 없고 왕복 비용만 늘었습니다.

## 결과 (준비 3초, 측정 10초, 1회)

| 시나리오 | TTL | cache off p50 (ms) | cache on p50 (ms) | 적중률 | 원본 조회 off→on |
|---|---|---|---|---|---|
| 상세 집중, 99:1 | 60s | 1.61 | 4.22 | 53.1% | 4,956→2,322 |
| 상세 집중, 99:1 | 2s | 1.62 | 5.08 | 22.1% | 4,956→3,859 |
| 상세 집중, 90:10 | 60s | 3.67 | 6.32 | 48.5% | 4,517→2,324 |
| 상세 집중, 90:10 | 2s | 3.52 | 6.78 | 20.7% | 4,517→3,584 |
| 목록 다양, 90:10 | 60s | 13.20 | 16.22 | 0% | 4,540→4,540 |
| 검색 다양, 90:10 | 60s | 15.75 | 17.53 | 0% | 4,512→4,512 |

TTL 2초에서는 적중률이 53.1%→22.1%, 48.5%→20.7%로 떨어져 만료·재저장 비용이 드러납니다. 수정이 섞여도 상세 집중 조회는 원본 조회가 절반 이하로 줄었습니다. 목록·검색은 조건이 매번 달라 적중이 없고 왕복 비용만 늘었습니다. TTL 2초 cache off의 p95 72.59 ms는 실행 편차로 보이며, 원인을 단정하지 않습니다.
