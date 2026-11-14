<!-- readme: archetype=team -->
<p align="center">
  <img src="docs/images/mini-redis_README_banner.png" width="100%" alt="mini-redis 배너">
</p>

[![CI](https://github.com/benjohnbill/mini-redis/actions/workflows/ci.yml/badge.svg)](https://github.com/benjohnbill/mini-redis/actions/workflows/ci.yml)
![C](https://img.shields.io/badge/C-11-A8B9CC?logo=c&logoColor=white)
![Runtime deps](https://img.shields.io/badge/runtime%20deps-0-brightgreen)

Krafton Jungle Week 11-12: a key-value store on a hash table, usable through an API

[설치 및 실행](#설치-및-실행) · [API](#api) · [응답 캐시 데모](#응답-캐시-데모) · [마일스톤](#마일스톤) · [알려진 한계](#알려진-한계) · [파일 트리](#파일-트리) · [문서](#문서)

## 설치 및 실행

gcc(또는 clang), make, python3가 필요합니다. `make verify`의 상품 데모는 SQLite C 개발 라이브러리도 사용합니다. macOS는 Xcode Command Line Tools의 SQLite를 쓰며, Ubuntu에서는 `sudo apt-get install libsqlite3-dev`로 설치합니다.

```bash
make verify              # 빌드, 테스트, ID 검사 (CI와 같은 명령)
make test                # 테스트만 실행
make bench               # 지연 측정 (wrk 필요: sudo apt install wrk). make verify와 CI에는 들어가지 않습니다
./build/mini-redis 8080  # 서버 실행 (포트는 꼭 적습니다)
```

서버는 SIGTERM이나 SIGINT(Ctrl+C)를 받으면 끝납니다.

## API

다른 서버는 HTTP로 mini-redis를 부릅니다. 동작은 메서드로, key는 경로 `/keys/<key>`로, value는 body의 바이트 그대로 나타냅니다.

| 요청 | 동작 |
|---|---|
| `PUT /keys/<key>` | body를 value로 저장합니다. 같은 key가 있으면 덮어씁니다. `TTL` 헤더로 보관 기간을 정할 수 있습니다. |
| `GET /keys/<key>` | 저장한 value를 body로 돌려줍니다. |
| `DELETE /keys/<key>` | key를 지웁니다. |

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

key 인코딩, `TTL` 헤더의 규칙, body와 연결의 상한, 모든 상태 코드와 오류 이유, 오류의 우선순위는 [`docs/api.md`](docs/api.md)에 있습니다.

## 응답 캐시 데모

M5에서 mini-redis를 상품 API의 응답 캐시로 쓰는 별도 실행 파일 `cache-demo`를 만들었습니다. 같은 조건을 반복하는 목록 조회는 캐시를 켜면 p50이 10.2 ms에서 2.7 ms로 줄었고, 상세 조회는 mini-redis 왕복을 기다리느라 1.2 ms에서 3.6~4.0 ms로 늘었습니다.

- 실행 방법과 HTTP 계약: [`demo/README.md`](demo/README.md)
- 측정 조건과 전체 결과: [`docs/milestones/cache/spec.md`의 "측정 결과"](docs/milestones/cache/spec.md#측정-결과)

## 마일스톤

| 순서 | 이름 | 주제 | 요구사항 | 상태 | Redis와 대조 |
|---|---|---|---|---|---|
| M0 | — | 준비: Pintos `hash.c` 이식, 요청을 하나씩 받는 서버, 테스트 도구, CI | — | 완료 | — |
| M1 | [`store`](docs/milestones/store/overview.md) | 저장, 조회, 삭제 | L2 L4 L13 | 완료 | [대조](docs/milestones/store/spec.md#redis와-대조) |
| M2 | [`api`](docs/milestones/api/overview.md) | 외부에서 부르기 | L3 L10 L14 | 진행 중 | |
| M3 | [`concurrency`](docs/milestones/concurrency/overview.md) | 동시성 | L8 | 진행 중 | [대조](docs/milestones/concurrency/spec.md#redis와-대조) |
| M4 | [`expire`](docs/milestones/expire/overview.md) | 보관 기간 | L9 | 진행 중 | [대조](docs/milestones/expire/spec.md#redis와-대조) |
| M5 | [`cache`](docs/milestones/cache/spec.md) | API 응답 캐시, 무효화, 성능 비교 | L15 L11 L17 | 진행 중 | [대조](docs/milestones/cache/spec.md#redis와-대조) |
| M6 | `persist` | 서버가 꺼져도 데이터 유지 (선택) | L12 | 진행 전 | |

요구사항 ID는 [`docs/requirements.md`](docs/requirements.md)에 있습니다. L16(엣지 케이스)은 모든 마일스톤의 테스트에 들어갑니다. 마일스톤이 끝나면 `Redis와 대조` 칸에 그 `spec.md`의 `Redis와 대조` 절을 링크합니다.

## 알려진 한계

- eviction(메모리가 가득 찰 때 key를 내보내는 정책)은 LMS에 없어 구현하지 않습니다.
- **key 수가 2배로 늘어나는 순간 서버가 한 번 멈춥니다.** store가 약 13만 개를 넘으면 그 멈춤이 M3의 목표(작은 GET의 p99 10 ms 이하)를 넘고, 400만 개에서는 약 310~350 ms입니다([측정 결과](docs/milestones/concurrency/spec.md#측정-결과)).
- **16 MiB value를 저장하는 동안 서버가 약 12 ms 멈춥니다.** 상한 크기의 value를 초당 5번 저장하면 작은 GET의 p99가 목표를 약 1 ms 넘습니다([측정 결과](docs/milestones/concurrency/spec.md#측정-결과)).
- **요청이 없어도 서버가 초당 40번 깨어납니다.** 주기 청소가 25 ms 간격을 지키기 때문입니다([측정 결과](docs/milestones/expire/spec.md#측정-결과)).
- **key의 해시에 무작위 seed를 섞지 않습니다.** 같은 버킷에 떨어지는 key를 골라 넣으면 조회가 느려집니다([Redis와 대조](docs/milestones/store/spec.md#redis와-대조)).
- **보관 기간과 연결에는 client가 알아야 할 한계가 있습니다.** 절전 중 멈추는 시계, 전송 지연만큼 늦는 만료, 느린 연결, `503` 뒤의 연결 리셋, 끊긴 body의 저장입니다([`docs/api.md`의 "한계"](docs/api.md#한계)).
- **응답 캐시 데모는 백엔드 한 프로세스와 넓은 무효화로 제한합니다.** 세부는 [`demo/README.md`의 "한계"](demo/README.md#한계)에 있습니다.

## 파일 트리

```
mini-redis/
├─ Makefile          make verify, make test, make bench
├─ src/              소스 코드
├─ demo/             상품 데모(M5)와 그 README.md
├─ tests/            <마일스톤 이름>_test.c, functional/
├─ scripts/          check-ids.py, convention.py, bench/(지연 측정)
├─ docs/
│  ├─ api.md            HTTP 계약 전체
│  ├─ requirements.md   LMS 요구사항과 우리 해석
│  ├─ milestones/       마일스톤마다 overview.md, spec.md, issues/
│  ├─ adr/              큰 설계 결정
│  └─ agents/           AI 스킬 설정
└─ .github/workflows/ci.yml
```

## 문서

- [`GLOSSARY.md`](GLOSSARY.md): 코드와 문서에서 쓰는 용어
- [`CONVENTION.md`](CONVENTION.md): 팀 규칙, 마일스톤 6단계, 문서 역할
- [`docs/api.md`](docs/api.md): HTTP 계약 전체 (client 작성자용)
- [`demo/README.md`](demo/README.md): 상품 데모의 실행 방법과 HTTP 계약
- [`docs/requirements.md`](docs/requirements.md): 요구사항 표
- [`AGENTS.md`](AGENTS.md): AI 에이전트가 먼저 읽는 파일

### Environment

- 컴파일러: gcc 또는 clang (C11)
- 빌드: make
- ID 검사: python3

### Team

이민호 · 안다빈 · 조진근

### 상표

이 프로젝트는 Krafton Jungle 과제로 만든 학습용 구현이며, Redis Ltd.가 보증, 지원, 인증하지 않습니다. This project is not endorsed, supported, or certified by Redis.

Redis® is a registered trademark of Redis Ltd. Any rights therein are reserved to Redis Ltd.
