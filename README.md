<!-- readme: archetype=team -->
<p align="center">
  <img src="docs/images/mini-redis_README_banner.png" width="100%" alt="mini-redis 배너">
</p>

[![CI](https://github.com/benjohnbill/mini-redis/actions/workflows/ci.yml/badge.svg)](https://github.com/benjohnbill/mini-redis/actions/workflows/ci.yml)
![C](https://img.shields.io/badge/C-11-A8B9CC?logo=c&logoColor=white)
![Runtime deps](https://img.shields.io/badge/runtime%20deps-0-brightgreen)

Krafton Jungle Week 11-12: a key-value store on a hash table, usable through an API

[설치 및 실행](#설치-및-실행) · [파일 트리](#파일-트리) · [마일스톤](#마일스톤) · [알려진 한계](#알려진-한계) · [문서](#문서)

## 설치 및 실행

gcc(또는 clang), make, python3가 필요합니다.

```bash
make verify              # 빌드, 테스트, ID 검사 (CI와 같은 명령)
make test                # 테스트만 실행
./build/mini-redis 8080  # 서버 실행 (포트를 생략하면 8080)
```

지금 서버는 요청을 하나씩 처리하고, 어떤 요청에도 `ok`를 돌려줍니다.

```console
$ curl -i http://127.0.0.1:8080/
HTTP/1.1 200 OK
Content-Type: text/plain
Content-Length: 3
Connection: close

ok
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
| M1 | [`store`](docs/milestones/store/overview.md) | 저장, 조회, 삭제 | L2 L4 L13 | 진행 전 | |
| M2 | `api` | 외부에서 부르기 | L3 L10 L14 | 진행 전 | |
| M3 | `concurrency` | 동시성 | L8 | 진행 전 | |
| M4 | `expire` | 보관 기간 | L9 | 진행 전 | |
| M5 | `cache` | API 응답 캐시, 무효화, 성능 비교 | L15 L11 L17 | 진행 전 | |
| M6 | `persist` | 서버가 꺼져도 데이터 유지 (선택) | L12 | 진행 전 | |

요구사항 ID는 [`docs/requirements.md`](docs/requirements.md)에 있습니다. L16(엣지 케이스)은 모든 마일스톤의 테스트에 들어갑니다. 마일스톤이 끝나면 `Redis와 대조` 칸에 그 `spec.md`의 `Redis와 대조` 절을 링크합니다.

## 알려진 한계

- eviction(메모리가 가득 찰 때 key를 내보내는 정책)은 LMS에 없어 구현하지 않습니다.

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
