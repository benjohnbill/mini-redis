# 08: 선택 PostgreSQL 원본으로 같은 API와 로컬 기준선을 실행한다

**What to build:** 팀원이 같은 컴퓨터의 PostgreSQL을 원본으로 선택해 같은 상품 API·캐시·원본 변경 동작을 사용하고, 조회 전용 cache on/off 결과를 SQLite와 구분해 비교합니다.

**Blocked by:** 04 — 상품 수정과 삭제 뒤 상세·목록·검색 캐시를 무효화한다; 06 — SQLite 원본의 조회 전용 캐시 효과를 재현 가능하게 잰다.

Status: ready-for-agent

- [x] libpq를 사용하는 선택 빌드와 원본 연결 입력을 제공한다. 기본 SQLite 빌드·make verify는 PostgreSQL 개발 라이브러리나 실행 중인 PostgreSQL을 요구하지 않는다.
- [x] 동일한 상품·id·가격 범위·정렬 동률·부분 문자열 검색·페이지 의미와 인덱스를 유지하며 연결을 재사용한다. 질의 기한과 DB 오류는 문서화한다.
- [x] 같은 HTTP 경계에서 상세·목록·검색의 on/off, 수정·삭제 뒤 무효화와 원본 조회 카운터를 검증할 수 있다. 실제 PostgreSQL이 제공된 경우 검증 결과를 남긴다.
- [x] 06의 요청 계획과 출력 형식으로 로컬 PostgreSQL의 조회 전용 cache on/off 실험을 실행할 수 있다. SQLite와 PostgreSQL을 별도의 원본 환경으로 표시한다.
- [x] DB 버전·장비·프로세스 배치·쿼리와 인덱스·DB 및 캐시 연결 방식·실행 명령을 기록한다. DB 종류 차이를 순수한 통신 비용으로 해석하지 않는다.
- [x] 선택 환경의 준비·종료와 연결 정보 입력 방법을 문서화한다. 실행 환경이 없으면 실제 PostgreSQL 검증·측정은 미실행으로 표시하며 결과를 만들지 않는다.
- [x] 선택 DB가 없더라도 기본 기능 검증과 SQLite 실험은 그대로 실행된다.

## Comments

- 기본 데모 뒤의 추가 실험이다. 로컬 PostgreSQL은 다른 컴퓨터를 거친 통신 실험으로 표현하지 않는다. 원격 접속과 혼합 부하의 추가 환경 비교는 09에서 수행한다.

## 구현과 검증

- `demo/origin.h`로 상품 조회·목록·수정·삭제·준비를 나누고, SQLite 구현을 `demo/sqlite_origin.c`로 옮겼습니다. `demo/product_demo.c`는 HTTP와 캐시만 다룹니다. 기본 `make cache-demo`와 `make verify`는 계속 SQLite만 링크합니다.
- `demo/postgres_origin.c`는 libpq로 연결을 재사용하고 `statement_timeout = 1000ms`를 설정합니다. `--db`가 `postgresql://` 연결 문자열이면 이 원본을 씁니다. 스키마는 같은 `id` key, `(price,id)` 인덱스, 1~120바이트 이름, 0~1,000,000,000 가격이고 `--seed`가 표를 만들고 기존 행을 지웁니다.
- 선택 빌드·실행은 `make cache-demo-postgres`, `make cache-functional-postgres POSTGRES_URL=...`, `make cache-bench-postgres POSTGRES_URL=...`입니다. 기본 빌드·CI·make verify는 libpq나 실행 중인 DB를 요구하지 않습니다.
- 실제 PostgreSQL 16.15(Homebrew, 같은 컴퓨터, DB는 별도 프로세스)에서 `make cache-functional-postgres`로 상세 캐시·목록·검색·수정·삭제 무효화 3개 테스트가 통과했습니다.
- 조회 전용 측정: 상품 10,000개 seed 1, 연결 32, 초당 500요청, 준비 3초, 측정 10초, 1회, TTL 60초, `python3 -B scripts/bench/cache_bench.py build/bench/cache-demo-postgres build/bench/mini-redis --db 'postgresql://127.0.0.1/mini_redis_bench' --warmup 3 --measure 10 --runs 1 --ttl 60 --scenario ...`. 상세 균등 p50 4.69→6.88ms(적중률 35.9%, 원본 5,024→3,220), 목록 반복 7.07→2.83ms(100%, 5,024→0), 검색 다양 25.23→36.46ms(0%, 5,024→5,024)입니다.
- SQLite와 PostgreSQL은 별도 원본 환경입니다. 이 차이는 네트워크 하나의 효과가 아니며, DB 종류·질의·연결 배치 차이를 함께 봐야 합니다. `/stats`의 `origin` 값으로 실행 원본을 구분합니다.
