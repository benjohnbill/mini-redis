<!-- readme: archetype=team -->
<p align="center">
  <img src="docs/images/mini-redis_README_banner.png" width="100%" alt="mini-redis 배너">
</p>

[![CI](https://github.com/benjohnbill/mini-redis/actions/workflows/ci.yml/badge.svg)](https://github.com/benjohnbill/mini-redis/actions/workflows/ci.yml)
![C](https://img.shields.io/badge/C-11-A8B9CC?logo=c&logoColor=white)
![Runtime deps](https://img.shields.io/badge/runtime%20deps-0-brightgreen)

Krafton Jungle 11–12주차 프로젝트입니다. C로 만든 키-값 저장소를 상품 API의 응답 캐시로 사용합니다.

[빠른 시작](#빠른-시작) · [API](#api) · [시연](#4-시연-같은-상품을-두-번-조회하면) · [성능 분석](#5-성능-분석) · [마일스톤](#마일스톤) · [문서](#문서)

## 1. 왜 캐시를 쓰나요?

여러 사람이 같은 상품 목록을 열면 서버는 같은 조건으로 DB를 조회합니다. 첫 응답을 캐시에 저장해 두면 다음 요청에는 저장된 응답을 돌려주고 DB 조회를 건너뛸 수 있습니다.

| 만든 것 | 하는 일 |
|---|---|
| mini-redis (`src/`) | HTTP로 key에 값을 저장하고, 읽고, 지울 수 있습니다. key마다 보관 기간(TTL)을 정할 수 있습니다. |
| 상품 데모 (`demo/`) | 상품 상세, 목록, 검색 API입니다. 원본 DB(SQLite 또는 PostgreSQL)에서 만든 응답을 mini-redis에 저장해 다시 사용합니다. |

응답 시간은 DB 조회를 얼마나 줄이는지와 캐시를 오가는 데 얼마나 걸리는지에 따라 달라집니다. [성능 분석](#5-성능-분석)에서 두 값을 함께 봅니다.

## 2. 개발 환경과 프로젝트 구조

### 기술 스택

| 영역 | 사용한 것 |
|---|---|
| 언어 | C11 |
| HTTP 서버 | [Mongoose](https://github.com/cesanta/mongoose) (`vendor/mongoose/`, 레포에 포함) |
| 해시 테이블 | Pintos `hash.c` 이식, key hash는 SipHash 1-2 |
| 상품 데모의 원본 DB | SQLite(기본) 또는 PostgreSQL(선택) |
| 테스트와 측정 | acutest(단위), Python 3(기능 테스트, 벤치마크), wrk |

### 프로젝트 구조

```
mini-redis/
├─ Makefile          make verify, make test, make bench, make cache-bench
├─ src/              mini-redis 서버 코드
├─ demo/             상품 데모(M5)와 그 README.md
├─ tests/            <마일스톤 이름>_test.c, functional/
├─ scripts/          check-ids.py, convention.py, bench/(측정 스크립트)
├─ docs/
│  ├─ api.md            HTTP 계약 전체
│  ├─ requirements.md   LMS 요구사항과 우리 해석
│  ├─ milestones/       마일스톤마다 overview.md, spec.md, issues/
│  ├─ measurements/     측정 기록
│  ├─ adr/              큰 설계 결정
│  └─ agents/           AI 스킬 설정
└─ .github/workflows/ci.yml
```

## 3. 값 하나를 어떻게 찾나요?

<p align="center">
  <img src="docs/images/mini-redis-architecture.png" width="100%" alt="mini-redis 구조: client의 HTTP 요청을 스레드 하나의 이벤트 루프가 받아 api 층과 store로 넘기고, 같은 루프가 주기 청소로 만료된 key를 지웁니다. store는 해시 테이블과 만료 heap을 씁니다.">
</p>

서버는 프로세스 1개, 스레드 1개로 모든 요청을 처리합니다. 그림의 원본은 [`docs/images/mini-redis-architecture.html`](docs/images/mini-redis-architecture.html)입니다.

`GET /keys/greeting`이 들어오면 다음 순서로 값을 찾습니다.

```text
client → server.c  HTTP 요청을 받음
       → api.c     경로에서 greeting을 꺼냄
       → store.c   hash.c에 key를 찾도록 요청
           ↳ hash.c  SipHash로 검색할 버킷을 고르고 그 안에서 key를 찾음
       ← store.c   찾은 값이 아직 보관 기간 안에 있는지 확인
client ← 200과 저장한 값, 없거나 만료됐으면 404 key_not_found
```

### 설계 결정

| 주제 | 고른 것 | 이유와 기록 |
|---|---|---|
| HTTP 요청 형식 | `PUT`·`GET`·`DELETE /keys/<key>`, value는 body 바이트 그대로 | JSON으로 감싸지 않아 이미지 같은 바이트도 그대로 저장합니다. [ADR `http-methods-and-raw-body`](docs/adr/http-methods-and-raw-body.md) |
| 해시 테이블 | Pintos `hash.c`, 무작위 seed를 사용하는 SipHash 1-2 | 서버 밖에서 어떤 key가 같은 버킷에 들어갈지 예측하기 어렵게 합니다. [ADR `siphash-with-random-seed`](docs/adr/siphash-with-random-seed.md) |
| 해시 테이블을 늘릴 때 | key와 값을 묶은 entry를 여러 요청에 걸쳐 옮기는 점진적 rehash | 한 번에 옮기면 그동안 다른 요청이 수백 ms 기다립니다. [5. 성능 분석](#개선-사례-테이블이-커질-때-작은-get이-기다리던-문제) |
| 동시 요청 | 스레드 1개의 이벤트 루프 | store를 동시에 부르지 않으므로 잠금이 필요 없습니다. [ADR `single-thread-event-loop`](docs/adr/single-thread-event-loop.md) |
| 만료 | `TTL` 헤더, 만료가 가까운 key부터 확인하는 자료구조(heap), 주기 청소 | 조회되지 않는 만료 key도 메모리에서 지웁니다. [ADR `ttl-in-request-header`](docs/adr/ttl-in-request-header.md), [ADR `expiry-heap-with-immediate-removal`](docs/adr/expiry-heap-with-immediate-removal.md) |
| 상품 수정 시 캐시 무효화 | 데모 백엔드가 그 상품의 상세 캐시와 목록·검색 캐시 전체를 무효화 | 상품 변경이 어느 응답에 영향을 주는지는 데모 백엔드가 판단합니다. [ADR `cache-invalidation-owned-by-demo-backend`](docs/adr/cache-invalidation-owned-by-demo-backend.md) |

## 4. 시연: 같은 상품을 두 번 조회하면

상품 데모는 상품 요청을 받으면 먼저 mini-redis에 응답이 있는지 묻습니다.

```text
첫 요청(MISS)    client → 상품 API → 캐시에 없음 → DB에서 조회해 응답을 만들고 캐시에 저장
두 번째 요청(HIT) client → 상품 API → 캐시에서 찾아 응답
```

터미널 1에서 두 프로그램을 빌드하고 mini-redis를 실행합니다.

```bash
make build/mini-redis cache-demo
./build/mini-redis 8080
```

터미널 2에서 시연용 DB에 상품 10,000개를 넣고 상품 API를 실행합니다.

```bash
demo_dir=$(mktemp -d)
./build/cache-demo --db "$demo_dir/products.sqlite" --seed 1 --count 10000
./build/cache-demo --db "$demo_dir/products.sqlite" --port 8081 --cache on \
  --cache-url http://127.0.0.1:8080 --ttl-seconds 60
```

터미널 3에서 1번 상품을 조회한 직후, 60초 안에 같은 상품을 다시 조회합니다. 각 응답을 받은 뒤 `/stats`에서 원본 조회 횟수를 확인합니다.

```bash
curl http://127.0.0.1:8081/products/1   # 첫 조회: MISS
curl http://127.0.0.1:8081/stats
curl http://127.0.0.1:8081/products/1   # 두 번째 조회: HIT
curl http://127.0.0.1:8081/stats
```

| 조회 | 캐시 결과 | 이번 요청의 DB 조회 | `/stats`의 누적 `origin_queries` |
|---|---|---:|---:|
| 첫 조회 | MISS · DB 응답을 캐시에 저장 | 1회 | 1 |
| 두 번째 조회 | HIT · 저장한 응답을 반환 | 0회 | 1 |

실행 옵션, 상품 API 전체, 무효화 규칙은 [`demo/README.md`](demo/README.md)에 있습니다.

## 5. 성능 분석

### 테스트가 확인하는 것

| 무엇을 띄우나 | 확인하는 것 | 명령 |
|---|---|---|
| mini-redis만 | 저장, 조회, 삭제, 만료의 동작과 오류 응답 | `make verify` |
| mini-redis만 | 큰 value, rehash, 만료 청소가 있을 때 작은 GET의 지연 | `make bench` |
| mini-redis + 상품 데모 | 캐시 저장, 무효화, 캐시 장애 뒤 재개 | `make verify` (`cache-functional`) |
| mini-redis + 상품 데모 | 같은 요청의 캐시 OFF/ON 응답 시간과 원본 조회 횟수 | `make cache-bench`, `make cache-bench-postgres` |

상품 데모 벤치의 기본 설정은 상세·목록·검색의 6가지 요청 패턴에 조회·수정 비율 3가지(100:0, 99:1, 90:10)를 조합한 18개 시나리오입니다. TTL 60초와 2초에서 캐시 OFF와 ON을 각각 3회씩 측정합니다.

### 개선 사례: 테이블이 커질 때 작은 GET이 기다리던 문제

해시 테이블은 entry가 많아지면 버킷 배열을 키우고 entry를 새 배열로 옮깁니다. 이전에는 이 일을 저장 한 번에 모두 했습니다. 서버가 스레드 하나를 쓰기 때문에, 옮기는 동안 받은 작은 조회 요청도 기다렸습니다.

| 단계 | 버킷을 늘리는 저장 한 번 (ms) | 그때 작은 GET p99 (ms) | 기록 |
|---|---|---|---|
| 개선 전 | 531\~637 | 440\~546 | [`server/013`](docs/measurements/server/013-CJG-b1-baseline.md) |
| entry를 나눠 옮김 (#37) | 29\~56 | 0.68\~1.13 | [`server/014`](docs/measurements/server/014-CJG-pr37-incremental-rehash.md) |
| 버킷 초기화도 나눔 (#49) | 0.21\~0.29 | 0.66\~0.90 | [`server/024`](docs/measurements/server/024-CJG-lazy-buckets.md) |

같은 노트북과 명령으로 각 단계를 3번 측정한 범위입니다. p99는 요청의 약 99%가 이 시간 안에 끝났다는 뜻입니다. 작은 GET의 p99 목표는 10 ms 이하였습니다. [단계별 전체 결과](docs/milestones/perf/spec.md#측정-결과)

### 최종 결과: 캐시는 언제 유리한가

같은 조건으로 반복한 검색과 매번 조건을 바꾼 검색을 비교합니다. 캐시를 껐을 때와 켰을 때의 원본 DB 조회 횟수와 조회 p95(요청의 약 95%가 끝난 시간)를 함께 보면, DB 조회를 줄인 효과가 응답 시간에도 나타났는지 알 수 있습니다.

최종 측정 결과는 집계 후 이 절에 추가합니다. [상품 데모 측정 기록](docs/measurements/cache-demo/)

## 6. 실행·API·테스트

### 빠른 시작

필요한 도구는 쓰는 명령에 따라 다릅니다. CI는 `make verify`에 필요한 도구만 설치합니다.

| 명령 | 필요한 도구 |
|---|---|
| 서버 빌드 | gcc 또는 clang(C11), make |
| 상품 데모 (SQLite) | 서버 빌드 도구, SQLite 개발 라이브러리 |
| `make verify` | 상품 데모 빌드 도구, Python 3, clang-format 21.1.8, clang-tidy 21.1.6 |
| `make bench` | 서버 빌드 도구, Python 3, wrk |
| `make cache-bench` | 상품 데모 빌드 도구, Python 3 |
| `make cache-bench-postgres` (선택) | 서버 빌드 도구, Python 3, PostgreSQL 서버, libpq 개발 파일(`pg_config`) |

```bash
sudo apt-get install libsqlite3-dev wrk              # Ubuntu. macOS는 Xcode Command Line Tools의 SQLite를 씁니다
pip install clang-format==21.1.8 clang-tidy==21.1.6  # CI와 같은 버전입니다(CONVENTION.md "코드")
```

```bash
make build/mini-redis    # 서버 빌드
./build/mini-redis 8080  # 8080 포트에서 실행
```

서버는 SIGTERM이나 SIGINT(Ctrl+C)를 받으면 끝납니다.

### API

HTTP 메서드로 저장·조회·삭제를 고르고, 경로 `/keys/<key>`에 key를 넣습니다. 저장할 값은 요청 본문(body)에 그대로 보냅니다.

| 요청 | 동작 |
|---|---|
| `PUT /keys/<key>` | body를 value로 저장합니다. 같은 key가 있으면 덮어씁니다. `TTL` 헤더로 보관 기간을 정할 수 있습니다. |
| `GET /keys/<key>` | 저장한 value를 body로 돌려줍니다. |
| `DELETE /keys/<key>` | key를 지웁니다. |
| `POST /mget` | body에 줄마다 적은 key(최대 64개)의 value를 한 응답으로 돌려줍니다. |
| `POST /mset` | body에 적은 key와 value 쌍(최대 64개)을 한 요청으로 저장합니다. |

성공하면 조회는 `200`과 value 바이트를, 저장과 삭제는 `204`를 받습니다. 오류는 상태 코드와 `{"error":"<오류 이유>"}` body로 알립니다.

저장합니다.

```console
$ curl -i -X PUT --data-binary "hello" http://127.0.0.1:8080/keys/greeting
HTTP/1.1 204 No Content

```

조회합니다.

```console
$ curl -i http://127.0.0.1:8080/keys/greeting
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Content-Length: 5

hello
```

key 인코딩, `TTL` 헤더의 규칙, `POST /mget`과 `POST /mset`의 body 모양, 상한, 모든 상태 코드와 오류 이유는 [`docs/api.md`](docs/api.md)에 있습니다.

### 검증과 재현

```bash
make verify                 # 빌드, 단위·기능 테스트, ID 검사, lint (CI와 같은 명령)
make test                   # 단위 테스트만
make bench                  # mini-redis 서버 지연 측정
make cache-bench            # 상품 데모의 캐시 OFF/ON 비교 (SQLite)
make cache-bench-postgres POSTGRES_URL='postgresql:///mini_redis_bench'   # 같은 비교 (PostgreSQL)
```

측정은 `make verify`와 CI에 들어가지 않습니다. 측정한 결과는 측정 한 번마다 파일 하나로 [`docs/measurements/`](docs/measurements/)에 남깁니다(`CONVENTION.md`의 "측정 기록"). PostgreSQL 측정 방법은 [`demo/README.md`의 "성능 비교"](demo/README.md#성능-비교)에 있습니다.

## 7. 프로젝트 기록

### 마일스톤

| 순서 | 이름 | 주제 | 요구사항 | 상태 | 측정 | Redis와 대조 |
|---|---|---|---|---|---|---|
| M0 | — | 준비: Pintos `hash.c` 이식, 요청을 하나씩 받는 서버, 테스트 도구, CI | — | 완료 | — | — |
| M1 | [`store`](docs/milestones/store/overview.md) | 저장, 조회, 삭제 | L2 L4 L13 | 완료 | | [대조](docs/milestones/store/spec.md#redis와-대조) |
| M2 | [`api`](docs/milestones/api/overview.md) | 외부에서 부르기 | L3 L10 L14 | 진행 중 | | |
| M3 | [`concurrency`](docs/milestones/concurrency/overview.md) | 동시성 | L8 | 진행 중 | [server/001\~003](docs/measurements/server/) | [대조](docs/milestones/concurrency/spec.md#redis와-대조) |
| M4 | [`expire`](docs/milestones/expire/overview.md) | 보관 기간 | L9 | 진행 중 | [server/004](docs/measurements/server/004-CJG-expire-sweep.md) | [대조](docs/milestones/expire/spec.md#redis와-대조) |
| M5 | [`cache`](docs/milestones/cache/spec.md) | API 응답 캐시, 무효화, 성능 비교 | L15 L11 L17 | 진행 중 | [cache-demo/001\~004](docs/measurements/cache-demo/) | [대조](docs/milestones/cache/spec.md#redis와-대조) |
| M6 | `persist` | 서버가 꺼져도 데이터 유지 (선택) | L12 | 진행 전 | | |
| M7 | [`perf`](docs/milestones/perf/overview.md) | 측정된 증상을 원인별로 줄이는 성능 개선 | L8 L17 | 진행 중 | [측정 결과](docs/milestones/perf/spec.md#측정-결과) | |

요구사항 ID는 [`docs/requirements.md`](docs/requirements.md)에 있습니다. L16(엣지 케이스)은 모든 마일스톤의 테스트에 들어갑니다. `측정` 칸은 그 마일스톤의 측정 기록을 링크합니다. 마일스톤이 끝나면 `Redis와 대조` 칸에 그 `spec.md`의 `Redis와 대조` 절을 링크합니다.

### 알려진 한계

- eviction(메모리가 가득 찰 때 key를 내보내는 정책)은 LMS에 없어 구현하지 않습니다.
- **16 MiB 값을 저장하면 함께 들어온 작은 GET이 기다릴 수 있습니다.** 성능 개선 단계별 측정에서도 이 조건의 작은 GET p99는 목표인 10 ms를 넘었습니다. [서버 측정 결과](docs/milestones/perf/spec.md#측정-결과)
- **보관 기간과 연결에는 client가 알아야 할 한계가 있습니다.** 절전 중 멈추는 시계, 전송 지연만큼 늦는 만료, 느린 연결, `503` 뒤의 연결 리셋입니다([`docs/api.md`의 "한계"](docs/api.md#한계)).
- **응답 캐시 데모는 백엔드 한 프로세스와 넓은 무효화로 제한합니다.** 세부는 [`demo/README.md`의 "한계"](demo/README.md#한계)에 있습니다.

### 문서

- [`GLOSSARY.md`](GLOSSARY.md): 코드와 문서에서 쓰는 용어
- [`CONVENTION.md`](CONVENTION.md): 팀 규칙, 마일스톤 6단계, 문서 역할, 측정 기록
- [`docs/api.md`](docs/api.md): HTTP 계약 전체 (client 작성자용)
- [`demo/README.md`](demo/README.md): 상품 데모의 실행 방법과 HTTP 계약
- [`docs/requirements.md`](docs/requirements.md): 요구사항 표
- [`docs/measurements/`](docs/measurements/): 측정 기록. 측정 한 번마다 파일 하나
- [`docs/adr/`](docs/adr/): 큰 설계 결정
- [`AGENTS.md`](AGENTS.md): AI 에이전트가 먼저 읽는 파일

### 팀

이민호 · 안다빈 · 조진근

### 상표

이 프로젝트는 Krafton Jungle 과제로 만든 학습용 구현이며, Redis Ltd.가 보증, 지원, 인증하지 않습니다. This project is not endorsed, supported, or certified by Redis.

Redis® is a registered trademark of Redis Ltd. Any rights therein are reserved to Redis Ltd.
