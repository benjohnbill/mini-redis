---
seq: 5
topic: chain-b2-baseline
date: 2026-11-18
who: CJG
commit: 409f6b9
previous: 004-CJG-sqlite-grid.md
machine: "Intel Core Ultra 7 155H(논리 코어 22개), Windows의 WSL2 Ubuntu 26.04.1(커널 6.18.58.1-microsoft-standard-WSL2, 메모리 15 GiB), gcc 15.2.0, Python 3.14.4, PostgreSQL 18.6(WSL에 apt로 설치, 유닉스 소켓)"
build: "make -B build/bench/cache-demo-postgres build/bench/mini-redis: -O2, sanitizer 끔"
command: "make cache-bench-postgres POSTGRES_URL='postgresql:///mini_redis_bench' POSTGRES_BENCH_FLAGS='--scenario detail-uniform --output 005-CJG-b2-baseline.json' (연결 32, 초당 500요청, burst, 준비 10초, 측정 30초, 3회, TTL 60초와 2초)"
milestones: [perf]
data: 005-CJG-b2-baseline.json
---

# 병합 사슬 B2: 데모 PR을 병합하기 전의 상품 데모 기준선

## 앞 기록과 달라진 점

- **목적:** 병합 사슬의 cache-demo 기준선입니다. 서버 PR(#37, #35, #44, #42, #36)을 모두 병합한 `main`에서, 데모 PR(#39, #40, #41)을 병합하기 전에 쟀습니다. 이후 cache-demo 사슬 기록은 바로 앞 단계와 견줍니다.
- **원본:** 004의 SQLite가 아니라 PostgreSQL입니다. #41이 PostgreSQL로 쟀기 때문입니다. 그래서 004의 숫자와 나란히 비교하지 않습니다.
- **PostgreSQL을 띄우는 방법:** WSL에 직접 설치한 PostgreSQL에 유닉스 소켓으로 접속했습니다. 같은 commit에서 준비 3초, 측정 10초, 1회로 견주면, Docker 컨테이너에 `127.0.0.1`로 접속할 때 캐시 off p50이 19.17 ms, 컨테이너 주소로 직접 접속할 때 12.65 ms, 직접 설치한 쪽이 9.27 ms였습니다. Docker의 포트 전달이 원본 조회를 느리게 하므로, 이 사슬은 직접 설치한 PostgreSQL로 잽니다.
- **시나리오:** perf `overview.md`의 증상("상세 조회에서 캐시를 켜면 느려짐")에 맞춰 `detail-uniform` 하나만 잽니다.

## 결론

목표는 "적중률이 50% 이상인 시나리오에서 cache on의 p50이 cache off의 p50보다 크지 않다"입니다.

| TTL | off p50, 3회 (ms) | on p50, 3회 (ms) | 적중률 | 판정 |
|---|---|---|---|---|
| 60초 | 8.69, 9.18, 8.84 | 10.03, 9.39, 9.14 | 71.8% | 초과 (3회 모두 on이 느림) |
| 2초 | 8.60, 8.70, 8.70 | 16.64, 16.36, 17.39 | 11.2\~11.3% | 대상 아님 (적중률 50% 미만) |

- **적중률이 71.8%인데도 캐시를 켜면 느립니다.** 데모가 몰려 온 요청 32개를 하나씩 처리하면서, 요청마다 mini-redis에 캐시 GET을 보내고 답을 기다리기 때문으로 봅니다. #39\~#41이 이 왕복과 응답 대기를 줄이려는 PR입니다.
- **이 노트북의 원본 조회는 DA 머신보다 느립니다.** PostgreSQL 조회 하나의 왕복이 `pgbench`로 평균 0.238 ms였습니다. DA가 잰 commit(`4c31d1d`)을 이 노트북에서 같은 조건으로 재면 캐시 off p50이 5.96 ms로, DA 머신의 1.96 ms보다 약 3배 컸습니다. 사슬은 같은 노트북에서만 견줍니다.

## 결과

`cache_bench.py`의 출력이며, 시간 단위는 ms입니다. 요청은 TTL마다 회당 15,008개이고 처리량은 모두 초당 500.3개, 오류와 timeout은 0입니다.

| TTL | 캐시 | 회 | p50 | p95 | p99 | 적중률 | 원본 조회 |
|---|---|---|---|---|---|---|---|
| 60초 | off | 1 | 8.69 | 11.20 | 14.57 | | 15,008 |
| | off | 2 | 9.18 | 12.92 | 18.11 | | 15,008 |
| | off | 3 | 8.84 | 11.92 | 16.14 | | 15,008 |
| | on | 1 | 10.03 | 15.86 | 18.35 | 71.8% | 4,239 |
| | on | 2 | 9.39 | 15.36 | 17.17 | 71.8% | 4,239 |
| | on | 3 | 9.14 | 14.24 | 16.85 | 71.8% | 4,239 |
| 2초 | off | 1 | 8.60 | 10.98 | 12.40 | | 15,008 |
| | off | 2 | 8.70 | 12.11 | 16.58 | | 15,008 |
| | off | 3 | 8.70 | 11.06 | 13.53 | | 15,008 |
| | on | 1 | 16.64 | 22.55 | 25.64 | 11.3% | 13,316 |
| | on | 2 | 16.36 | 20.68 | 23.99 | 11.3% | 13,316 |
| | on | 3 | 17.39 | 23.74 | 28.36 | 11.3% | 13,315 |
