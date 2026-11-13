<!-- readme: archetype=team -->
<p align="center">
  <img src="docs/images/mini-redis_README_banner.png" width="100%" alt="mini-redis 배너">
</p>

[![CI](https://github.com/benjohnbill/mini-redis/actions/workflows/ci.yml/badge.svg)](https://github.com/benjohnbill/mini-redis/actions/workflows/ci.yml)
![C](https://img.shields.io/badge/C-11-A8B9CC?logo=c&logoColor=white)
![Runtime deps](https://img.shields.io/badge/runtime%20deps-0-brightgreen)

Krafton Jungle Week 11-12: a key-value store on a hash table, usable through an API

[설치 및 실행](#설치-및-실행) · [API](#api) · [파일 트리](#파일-트리) · [마일스톤](#마일스톤) · [알려진 한계](#알려진-한계) · [문서](#문서)

## 설치 및 실행

gcc(또는 clang), make, python3가 필요합니다.

```bash
make verify              # 빌드, 테스트, ID 검사 (CI와 같은 명령)
make test                # 테스트만 실행
./build/mini-redis 8080  # 서버 실행 (포트는 꼭 적습니다)
```

서버는 SIGTERM이나 SIGINT(Ctrl+C)를 받으면 끝납니다.

## API

다른 서버는 HTTP로 mini-redis를 부릅니다. 동작은 메서드로, key는 경로 `/keys/<key>`로, value는 body의 바이트 그대로 나타냅니다.

| 요청 | 동작 |
|---|---|
| `PUT /keys/<key>` | body를 value로 저장합니다. 같은 key가 있으면 덮어씁니다. |
| `GET /keys/<key>` | 저장한 value를 body로 돌려줍니다. |
| `DELETE /keys/<key>` | key를 지웁니다. |

- **key:** `/keys/` 뒤의 나머지 전부입니다. 퍼센트 인코딩(`%HH`)을 정확히 한 번 풀고, 그 바이트 그대로 key로 씁니다. 그래서 `%2F`는 경로 구분자가 아니라 key 안의 `/`이고, `%00`이 든 key도 잘리지 않으며, `%2541`은 `%41`이라는 key가 됩니다. 한글, 공백, `/`가 든 key는 인코딩해서 보냅니다.
- **value:** body의 바이트 그대로이고, 길이는 `Content-Length`로 알립니다. JSON으로 감싸지 않습니다. `'\0'`이나 UTF-8이 아닌 바이트가 든 value(이미지 등)도 그대로 저장하고 돌려받습니다. 0바이트 value도 저장할 수 있고, 조회하면 `200`과 0바이트 body를 받습니다.
- **body 상한:** 16 MiB(16,777,216바이트)입니다. `Content-Length`가 이 값을 넘으면 서버는 body를 받기 전에 `413`을 보내고 연결을 닫습니다.

### 응답

| 상황 | 상태 코드 | body |
|---|---|---|
| 조회 성공 | `200` | value 바이트 (`Content-Type: application/octet-stream`) |
| 저장 성공 (새로 만듦, 덮어씀 구분 없음) | `204` | 없음 |
| 삭제 성공 | `204` | 없음 |
| 조회, 삭제할 key가 없음 | `404` | `{"error":"key_not_found"}` |
| `/keys/`로 시작하지 않는 경로 (`/keys`도 포함) | `404` | `{"error":"route_not_found"}` |
| 0바이트 key (`/keys/`) | `400` | `{"error":"empty_key"}` |
| 깨진 퍼센트 인코딩 (`%G1`, 끝이 잘린 `%2`) | `400` | `{"error":"bad_key_encoding"}` |
| `PUT`, `GET`, `DELETE`가 아닌 메서드 | `405` | `{"error":"method_not_allowed"}` |
| `Content-Length`가 16 MiB를 넘음 | `413` | `{"error":"body_too_large"}` |
| 서버의 메모리 부족 | `500` | `{"error":"out_of_memory"}` |

오류 응답의 `Content-Type`은 `application/json`입니다. 상태 코드가 같은 오류도 body의 오류 이유로 나눌 수 있습니다.

**오류 이유:** `key_not_found`, `route_not_found`, `empty_key`, `bad_key_encoding`, `method_not_allowed`, `body_too_large`, `out_of_memory`.

**오류의 우선순위:** 한 요청에 오류가 여럿이면 처음 걸린 하나로 답합니다.

1. `Content-Length`가 16 MiB를 넘으면, 경로와 메서드를 보기 전에 `413`입니다. 헤더가 도착하자마자 검사하기 때문입니다.
2. 경로가 `/keys/`로 시작하지 않으면 `404 route_not_found`입니다.
3. 메서드가 `PUT`, `GET`, `DELETE`가 아니면 `405`입니다.
4. key의 인코딩이 깨졌으면 `400 bad_key_encoding`, key가 비었으면 `400 empty_key`입니다.

예를 들어 `POST /nope`는 `404 route_not_found`이고, `POST /keys/`는 `405`입니다.

### 예시

서버를 `./build/mini-redis 8080`으로 띄운 뒤 보낸 요청과 실제 응답입니다.

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

삭제하고, 다시 조회합니다.

```console
$ curl -i -X DELETE http://127.0.0.1:8080/keys/greeting
HTTP/1.1 204 No Content

$ curl -i http://127.0.0.1:8080/keys/greeting
HTTP/1.1 404 Not Found
Content-Type: application/json
Content-Length: 25

{"error":"key_not_found"}
```

## 파일 트리

```
mini-redis/
├─ Makefile          make verify, make test
├─ src/              소스 코드
├─ tests/            <마일스톤 이름>_test.c, functional/
├─ scripts/          check-ids.py
├─ docs/
│  ├─ requirements.md   LMS 요구사항과 우리 해석
│  ├─ milestones/       마일스톤마다 overview.md, spec.md, issues/
│  ├─ adr/              큰 설계 결정
│  └─ agents/           AI 스킬 설정
└─ .github/workflows/ci.yml
```

## 마일스톤

| 순서 | 이름 | 주제 | 요구사항 | 상태 | Redis와 대조 |
|---|---|---|---|---|---|
| M0 | — | 준비: Pintos `hash.c` 이식, 요청을 하나씩 받는 서버, 테스트 도구, CI | — | 완료 | — |
| M1 | [`store`](docs/milestones/store/overview.md) | 저장, 조회, 삭제 | L2 L4 L13 | 완료 | [대조](docs/milestones/store/spec.md#redis와-대조) |
| M2 | [`api`](docs/milestones/api/overview.md) | 외부에서 부르기 | L3 L10 L14 | 진행 전 | |
| M3 | `concurrency` | 동시성 | L8 | 진행 전 | |
| M4 | `expire` | 보관 기간 | L9 | 진행 전 | |
| M5 | `cache` | API 응답 캐시, 무효화, 성능 비교 | L15 L11 L17 | 진행 전 | |
| M6 | `persist` | 서버가 꺼져도 데이터 유지 (선택) | L12 | 진행 전 | |

요구사항 ID는 [`docs/requirements.md`](docs/requirements.md)에 있습니다. L16(엣지 케이스)은 모든 마일스톤의 테스트에 들어갑니다. 마일스톤이 끝나면 `Redis와 대조` 칸에 그 `spec.md`의 `Redis와 대조` 절을 링크합니다.

## 알려진 한계

- eviction(메모리가 가득 찰 때 key를 내보내는 정책)은 LMS에 없어 구현하지 않습니다.
- key의 해시(`hash_bytes`)에 서버마다 다른 무작위 seed를 섞지 않습니다. 같은 버킷에 떨어지는 key를 일부러 골라 많이 넣으면, 조회가 상수 시간이 아니라 그 버킷의 entry 수에 비례해 느려집니다. LMS에 없는 요구라 M1에서 다루지 않았습니다(`docs/milestones/store/spec.md`의 `Redis와 대조`).

## 문서

- [`GLOSSARY.md`](GLOSSARY.md): 코드와 문서에서 쓰는 용어
- [`CONVENTION.md`](CONVENTION.md): 팀 규칙, 마일스톤 6단계, 문서 역할
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
