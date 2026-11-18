# M7 `perf`: spec

이 파일에는 아직 `## 측정 결과`만 있습니다. 증상과 원인은 [`overview.md`](overview.md)에 있습니다.

## 측정 결과

### 병합 사슬 (2026-11-18)

성능 PR을 `main`에 하나씩 병합하고, 병합할 때마다 `main`을 같은 노트북, 같은 명령으로 다시 쟀습니다. 기록마다 `previous`가 바로 앞 단계를 가리키므로, 두 기록의 차이가 PR 하나의 효과입니다. 사슬의 끝에 출발점을 다시 재서 처음과 같은 숫자를 얻었으므로, 사슬을 재는 동안 노트북의 상태는 바뀌지 않았습니다([`server/021`](../../measurements/server/021-CJG-b1-end.md), [`cache-demo/009`](../../measurements/cache-demo/009-CJG-b2-end.md)). 다른 머신의 기록과는 숫자를 견주지 않습니다.

**서버** (`make bench`, a\~h 시나리오 3회, 목표: 작은 GET p99 10 ms 이하)

| 단계 | 얻은 것 | 치른 값 | 기록 |
|---|---|---|---|
| B1 기준선 (`13a903a`) | e 버킷을 늘리는 저장 531\~637 ms, e p99 440\~546 ms, b p99 16.5\~26.6 ms로 둘 다 목표 초과 | | [`server/013`](../../measurements/server/013-CJG-b1-baseline.md) |
| #37 점진적 rehash | e 버킷을 늘리는 저장 29\~56 ms, e p99 0.68\~1.13 ms로 목표 통과 | RSS 약 1 MiB 증가 | [`server/014`](../../measurements/server/014-CJG-pr37-incremental-rehash.md) |
| #35 rehash 기준 (2, 1, 0.5) | 조회 하나가 16.5\~22.3% 빨라짐([`rehash-micro/002`](../../measurements/rehash-micro/002-CJG-pr35-on-main.md)). 서버 지연에서는 측정 해상도 아래 | c RSS +32 MiB, e RSS +96 MiB, e 버킷을 늘리는 저장 72\~110 ms로 약 2배, e p99가 목표를 넘는 회가 생김 | [`server/015`](../../measurements/server/015-CJG-pr35-rehash-thresholds.md) |
| #44 다음 기한까지 잠듦 | 요청이 없을 때 초당 깨어남 39.8번 → 1.0번([`idle-wakeup/004`](../../measurements/idle-wakeup/004-CJG-pr44-sleep-until-due.md)) | 부하 아래의 숫자는 같음 | [`server/016`](../../measurements/server/016-CJG-pr44-sleep-until-due.md) |
| #42 수신 버퍼를 한 번에 잡음 | b 16 MiB 저장 max 59.6\~91.1 ms → 35.1\~55.0 ms, d와 h RSS 약 12 MiB 감소 | h에 약 40\~50 ms 멈춤이 새로 생김. glibc mmap 기준값을 고정하면 사라짐([`server/019`](../../measurements/server/019-CJG-h-mmap-threshold.md)) | [`server/017`](../../measurements/server/017-CJG-pr42-recv-buffer.md) |
| #36 SipHash 1-2 | hash seed로 버킷을 밖에서 계산할 수 없게 됨 | 짧은 key에서는 보이지 않음 | [`server/018`](../../measurements/server/018-CJG-pr36-siphash.md) |
| #39\~#41 상품 데모를 위한 변경 | (상품 데모 표) | 서버의 숫자는 같음 | [`server/020`](../../measurements/server/020-CJG-pr39-41-demo-api.md) |

- **남은 목표 초과:** b(16 MiB 저장)의 p99는 사슬 내내 15\~26 ms입니다. e의 p99는 #35 뒤 버킷을 늘리는 멈춤(68\~141 ms)의 길이에 따라 1 ms와 51 ms 사이를 오갑니다.
- **남은 일:** #35가 키운 새 버킷 배열의 초기화(`rehash`의 `list_init`)를 나눠 하면 e의 멈춤이 줄어듭니다. #42 뒤 h의 멈춤은 `mallopt(M_MMAP_THRESHOLD, ...)`가 고치는 후보입니다. 둘 다 코드는 아직 바꾸지 않았습니다.

**상품 데모** (`make cache-bench-postgres`, `detail-uniform` 3회, 목표: 적중률 50% 이상에서 cache on의 p50이 off의 p50보다 크지 않음)

| 단계 | TTL 60초 off p50 (ms) | TTL 60초 on p50 (ms) | TTL 2초 on p50 (ms) | 기록 |
|---|---|---|---|---|
| B2 기준선 (`409f6b9`) | 8.69\~9.18 | 9.14\~10.03, 목표 초과 | 16.36\~17.39 | [`cache-demo/005`](../../measurements/cache-demo/005-CJG-b2-baseline.md) |
| #39 응답을 바로 전송 | 5.96\~6.04 | 5.59\~5.98, 아슬아슬하게 통과 | 10.07\~10.38 | [`cache-demo/006`](../../measurements/cache-demo/006-CJG-pr39-flush.md) |
| #40 `POST /mget` | 5.75\~5.95 | 3.20\~3.27 | 8.35\~8.39 | [`cache-demo/007`](../../measurements/cache-demo/007-CJG-pr40-mget.md) |
| #41 `POST /mset` | 5.70\~5.79 | 2.64\~2.78 | 5.84\~6.04, off와 같음 | [`cache-demo/008`](../../measurements/cache-demo/008-CJG-pr41-mset.md) |

- **TTL 60초(적중률 약 72%)에서 cache on의 p50이 off의 약 절반이 되었습니다.** 캐시 왕복이 회마다 조회 15,008번과 저장 4,239번에서 `POST /mget`과 GET 약 1,200번, `POST /mset`과 PUT 약 800번으로 줄었습니다.
- **원본은 WSL에 직접 설치한 PostgreSQL입니다.** Docker의 포트 전달이 원본 조회를 느리게 해서 캐시의 효과를 가렸기 때문입니다([`demo/README.md`](../../../demo/README.md)).
