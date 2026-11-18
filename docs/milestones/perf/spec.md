# M7 `perf`: spec

이 파일에는 아직 `## 측정 결과`만 있습니다. 증상과 원인은 [`overview.md`](overview.md)에 있습니다.

## 측정 결과

### 병합 사슬 (2026-11-18)

성능 PR을 `main`에 하나씩 병합하고, 병합할 때마다 `main`을 같은 노트북, 같은 명령으로 다시 쟀습니다. 기록마다 `previous`가 바로 앞 단계를 가리키므로, 두 기록의 차이가 PR 하나의 효과입니다. 사슬의 끝에 출발점을 다시 재서 처음과 같은 숫자를 얻었으므로, 사슬을 재는 동안 노트북의 상태는 바뀌지 않았습니다([`server/021`](../../measurements/server/021-CJG-b1-end.md), [`cache-demo/009`](../../measurements/cache-demo/009-CJG-b2-end.md)). 다른 머신의 기록과는 숫자를 견주지 않습니다.

**서버** (`make bench`, a\~h 시나리오 3회, 목표: 작은 GET p99 10 ms 이하)

단계마다 바뀐 지표만 모았습니다. 칸은 3회(#47은 5회)의 범위이고 단위는 ms입니다. 굵은 칸이 그 단계가 바꾼 지표입니다. 빈칸은 그 단계에서 재지 않은 지표입니다.

| 단계 | e 버킷을 늘리는 저장 | e 작은 GET p99 | b 16 MiB 저장 max | d 16 MiB 삭제 max | h 작은 GET max | 기록 |
|---|---|---|---|---|---|---|
| B1 기준선 | 531\~637 | 440\~546 | 69\~91 | 49\~53 | 4.2\~30.5 | [`013`](../../measurements/server/013-CJG-b1-baseline.md) |
| #37 점진적 rehash | **29\~56** | **0.68\~1.13** | 60\~73 | 43\~55 | 4.0\~7.2 | [`014`](../../measurements/server/014-CJG-pr37-incremental-rehash.md) |
| #35 rehash 기준 | **72\~110** | **1.0\~15.7** | 62\~81 | 40\~47 | 3.1\~7.9 | [`015`](../../measurements/server/015-CJG-pr35-rehash-thresholds.md) |
| #44 다음 기한까지 잠듦 | 84\~113 | 2.4\~20.5 | 60\~68 | 36\~43 | 5.2\~6.6 | [`016`](../../measurements/server/016-CJG-pr44-sleep-until-due.md) |
| #42 수신 버퍼 | 95\~117 | 2.8\~24.2 | **35\~55** | 39\~45 | **40\~44** | [`017`](../../measurements/server/017-CJG-pr42-recv-buffer.md) |
| #36 SipHash 1-2 | 103\~108 | 10.7\~14.6 | 35\~43 | 38\~44 | 40\~45 | [`018`](../../measurements/server/018-CJG-pr36-siphash.md) |
| #39\~#41 상품 데모 | 68\~141 | 1.2\~50.8 | 32\~101 | 47\~72 | 53\~136 | [`020`](../../measurements/server/020-CJG-pr39-41-demo-api.md) |
| #47 mmap 기준값 고정 | | | 35\~54 | **5.3\~6.5** | **4.3\~13.0** | [`022`](../../measurements/server/022-CJG-mmap-threshold-fix.md) |
| #49 버킷 초기화 나눔 | **0.21\~0.29** | **0.66\~0.90** | | | | [`024`](../../measurements/server/024-CJG-lazy-buckets.md) |
| B1 다시 잼 (끝 기준선) | 555\~679 | 465\~594 | 65\~107 | 41\~52 | 3.7\~6.7 | [`021`](../../measurements/server/021-CJG-b1-end.md) |

- **e는 버킷을 늘리는 저장 한 번이 서버를 붙잡는 시간입니다.** #37이 entry 옮기기를 나눠 약 600 ms를 약 40 ms로 줄였고, #35가 버킷 배열을 키워 약 100 ms로 늘렸고, #49가 남은 배열 초기화를 나눠 1 ms 아래로 줄였습니다.
- **h와 d는 16 MiB value를 해제할 때의 멈춤입니다.** #42 뒤 h에 약 40 ms 멈춤이 생겼고, #47이 h와 d의 멈춤을 함께 없앴습니다.
- **흔들린 칸:** 칸은 3회의 전체 범위이고, #47의 h만 측정 client가 멈춘 5회째(46.0 ms)를 뺐습니다. B1의 h 30.5 ms와 24.6 ms, #39\~#41의 b 101 ms와 h 136 ms도 같은 회에 측정 client가 멈춘 회라서, 그 칸의 위쪽 끝은 서버의 멈춤으로 읽지 않습니다.
- **#49의 e는 쉬는 연결 기한을 60초로 바꾼 binary로 쟀습니다.** 고친 `bench.py`로 다시 잰 [`023`](../../measurements/server/023-CJG-grow-delay.md)에서도 0.13\~0.56 ms였습니다.

| 단계 | 얻은 것 | 치른 값 | 기록 |
|---|---|---|---|
| B1 기준선 (`13a903a`) | e 버킷을 늘리는 저장 531\~637 ms, e p99 440\~546 ms, b p99 16.5\~26.6 ms로 둘 다 목표 초과 | | [`server/013`](../../measurements/server/013-CJG-b1-baseline.md) |
| #37 점진적 rehash | e 버킷을 늘리는 저장 29\~56 ms, e p99 0.68\~1.13 ms로 목표 통과 | RSS 약 1 MiB 증가 | [`server/014`](../../measurements/server/014-CJG-pr37-incremental-rehash.md) |
| #35 rehash 기준 (2, 1, 0.5) | 조회 하나가 16.5\~22.3% 빨라짐([`rehash-micro/002`](../../measurements/rehash-micro/002-CJG-pr35-on-main.md)). 서버 지연에서는 측정 해상도 아래 | c RSS +32 MiB, e RSS +96 MiB, e 버킷을 늘리는 저장 72\~110 ms로 약 2배, e p99가 목표를 넘는 회가 생김 | [`server/015`](../../measurements/server/015-CJG-pr35-rehash-thresholds.md) |
| #44 다음 기한까지 잠듦 | 요청이 없을 때 초당 깨어남 39.8번 → 1.0번([`idle-wakeup/004`](../../measurements/idle-wakeup/004-CJG-pr44-sleep-until-due.md)) | 부하 아래의 숫자는 같음 | [`server/016`](../../measurements/server/016-CJG-pr44-sleep-until-due.md) |
| #42 수신 버퍼를 한 번에 잡음 | b 16 MiB 저장 max 59.6\~91.1 ms → 35.1\~55.0 ms, d와 h RSS 약 12 MiB 감소 | h에 약 40\~50 ms 멈춤이 새로 생김. glibc mmap 기준값을 고정하면 사라짐([`server/019`](../../measurements/server/019-CJG-h-mmap-threshold.md)) | [`server/017`](../../measurements/server/017-CJG-pr42-recv-buffer.md) |
| #36 SipHash 1-2 | hash seed로 버킷을 밖에서 계산할 수 없게 됨 | 짧은 key에서는 보이지 않음 | [`server/018`](../../measurements/server/018-CJG-pr36-siphash.md) |
| #39\~#41 상품 데모를 위한 변경 | (상품 데모 표) | 서버의 숫자는 같음 | [`server/020`](../../measurements/server/020-CJG-pr39-41-demo-api.md) |
| #47 mmap 기준값 고정 | h와 d에서 16 MiB value를 해제할 때의 약 35\~40 ms 멈춤이 사라짐 | d의 16 MiB 삭제 p50 약 0.1 → 1 ms | [`server/022`](../../measurements/server/022-CJG-mmap-threshold-fix.md) |
| #48 e의 측정 시점 | 버킷을 늘리는 저장이 빠른 서버도 e가 끝까지 돎 | e 저장을 0초가 아니라 5초에 보냄 | [`server/023`](../../measurements/server/023-CJG-grow-delay.md) |
| #49 버킷 초기화 나눔 | e 버킷을 늘리는 저장 약 100 ms → 0.21\~0.29 ms, e p99 목표 통과 | 조회와 메모리는 같음. b, d, f, g, h는 재지 않음 | [`server/024`](../../measurements/server/024-CJG-lazy-buckets.md) |

- **남은 목표 초과:** b(16 MiB 저장)의 p99는 사슬 내내 15\~26 ms입니다. 16 MiB 저장 하나가 서버를 약 24 ms 붙잡기 때문입니다.

**상품 데모** (`make cache-bench-postgres`, `detail-uniform` 3회, 목표: 적중률 50% 이상에서 cache on의 p50이 off의 p50보다 크지 않음)

| 단계 | TTL 60초 off p50 (ms) | TTL 60초 on p50 (ms) | TTL 2초 on p50 (ms) | 기록 |
|---|---|---|---|---|
| B2 기준선 (`409f6b9`) | 8.69\~9.18 | 9.14\~10.03, 목표 초과 | 16.36\~17.39 | [`cache-demo/005`](../../measurements/cache-demo/005-CJG-b2-baseline.md) |
| #39 응답을 바로 전송 | 5.96\~6.04 | 5.59\~5.98, 아슬아슬하게 통과 | 10.07\~10.38 | [`cache-demo/006`](../../measurements/cache-demo/006-CJG-pr39-flush.md) |
| #40 `POST /mget` | 5.75\~5.95 | 3.20\~3.27 | 8.35\~8.39 | [`cache-demo/007`](../../measurements/cache-demo/007-CJG-pr40-mget.md) |
| #41 `POST /mset` | 5.70\~5.79 | 2.64\~2.78 | 5.84\~6.04, off와 같음 | [`cache-demo/008`](../../measurements/cache-demo/008-CJG-pr41-mset.md) |

- **TTL 60초(적중률 약 72%)에서 cache on의 p50이 off의 약 절반이 되었습니다.** 캐시 왕복이 회마다 조회 15,008번과 저장 4,239번에서 `POST /mget`과 GET 약 1,200번, `POST /mset`과 PUT 약 800번으로 줄었습니다.
- **원본은 WSL에 직접 설치한 PostgreSQL입니다.** Docker의 포트 전달이 원본 조회를 느리게 해서 캐시의 효과를 가렸기 때문입니다([`demo/README.md`](../../../demo/README.md)).
