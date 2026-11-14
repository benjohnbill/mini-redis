---
seq: 2
topic: local-postgres-read-only
date: 2026-11-14
who: LMH
commit: 82b93ad
previous: 001-LMH-sqlite-read-only.md
machine: "macOS 26.6.2 arm64, python 3.15.2, SQLite 3.54.1, PostgreSQL 16.15(Homebrew, 같은 컴퓨터의 별도 프로세스)"
build: "build/bench/cache-demo-postgres, build/bench/mini-redis: -O2, sanitizer 끔"
command: "make cache-bench-postgres POSTGRES_URL=... (준비 3초, 측정 10초, 1회, TTL 60초)"
milestones: [cache]
data: none
---

# 002 로컬 PostgreSQL 원본의 조회 전용 캐시 효과

## 앞 기록과 달라진 점

`src/`는 001과 같습니다. 원본을 SQLite에서 같은 컴퓨터의 PostgreSQL로 바꾼 선택 실험입니다(`demo/postgres_origin.c`). 이 기록은 `docs/milestones/cache/spec.md`의 `## 측정 결과`에서 옮겼습니다. 결과는 처음에 `README.md`에 적혔고(`82b93ad`), `f1bfa6d`에서 spec으로 옮겨졌습니다. `commit`은 결과를 처음 적은 커밋입니다.

## 결론

목록 반복은 캐시로 7.07→2.83 ms가 되었고, 상세 균등과 다양한 조건의 검색은 느려졌습니다. SQLite와 다른 원본 환경이므로 001과의 차이를 네트워크 비용 하나로 해석하지 않습니다.

## 결과

같은 컴퓨터의 PostgreSQL 16.15(Homebrew, 별도 프로세스)에서 [001](001-LMH-sqlite-read-only.md)과 같은 계획으로 1회씩 쟀습니다(준비 3초, 측정 10초, TTL 60초). 상세 균등은 p50 4.69→6.88 ms, 적중률 35.9%, 원본 5,024→3,220, 목록 반복은 7.07→2.83 ms, 적중률 100%, 원본 5,024→0, 검색 다양한 조건은 25.23→36.46 ms, 적중률 0%입니다. SQLite와 다른 원본 환경이므로 이 차이를 네트워크 비용 하나로 해석하지 않습니다.
