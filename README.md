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

gcc(또는 clang), make, python3가 필요합니다. `make verify`의 상품 데모는 SQLite C 개발 라이브러리도 사용합니다. macOS는 Xcode Command Line Tools의 SQLite를 쓰며, Ubuntu에서는 `sudo apt-get install libsqlite3-dev`로 설치합니다.

```bash
make verify              # 빌드, 테스트, ID 검사 (CI와 같은 명령)
make test                # 테스트만 실행
make bench               # 지연 측정 (wrk 필요: sudo apt install wrk). make verify와 CI에는 들어가지 않습니다
./build/mini-redis 8080  # 서버 실행 (포트는 꼭 적습니다)
```

서버는 SIGTERM이나 SIGINT(Ctrl+C)를 받으면 끝납니다.

### SQLite 상품 데모 (M5 기준선)

mini-redis와 별도 C 실행 파일입니다. 상세 응답을 실제 mini-redis에 저장하는 캐시 on 모드와 원본만 읽는 off 모드를 제공합니다. SQLite는 이 실행 파일에만 링크하며 `build/mini-redis`에는 링크하지 않습니다.

```bash
make cache-demo
# 준비 명령은 해당 DB의 products 내용을 트랜잭션 안에서 교체합니다.
./build/cache-demo --db /tmp/products.sqlite --seed 1 --count 10000
./build/cache-demo --db /tmp/products.sqlite --port 8081 --cache off
# 별도 터미널에서 ./build/mini-redis 8080 실행
./build/cache-demo --db /tmp/products.sqlite --port 8081 --cache on \
  --cache-url http://127.0.0.1:8080 --ttl-seconds 60 --cache-timeout-ms 100
# 다른 터미널에서 실행합니다.
curl http://127.0.0.1:8081/products/1
curl http://127.0.0.1:8081/stats
make cache-functional  # 임시 DB와 실제 HTTP 서버를 사용하는 sanitizer 기능 검사
```

준비 옵션은 `--db PATH --seed N [--count N]`입니다. seed는 1~2,147,483,646이며 count는 1~2,147,483,647, 생략하면 10,000입니다. id는 1부터 순서대로, 이름은 `상품 <id>`로 만듭니다. 가격은 seed를 시작값으로 매 상품마다 `state = state * 48271 % 2147483647`, `price = state % 1000000001`로 정합니다. seed 1의 첫 상품은 `{"id":1,"name":"상품 1","price":48271}`입니다. 상품 테이블의 기본 key는 id이고 `(price,id)` 인덱스를 둡니다.

서버 필수 옵션은 `--db PATH --port N --cache on|off`입니다. `--cache-url`은 HTTP 호스트와 선택 port만 받으며 기본값은 `http://127.0.0.1:8000`입니다. IPv6 URL은 지원하지 않습니다. `--ttl-seconds`는 기본 60초, `--cache-timeout-ms`는 기본 100ms이고 둘 다 양의 정수입니다. 캐시 기한은 연결부터 응답 완료까지 요청마다 적용합니다. 캐시 연결과 별도 Mongoose manager를 재사용하여 상품 요청 처리가 재진입하지 않도록 합니다. port는 1~65,535이고 localhost에서만 연결을 받습니다. 준비하지 않은 DB로 시작하면 종료 코드 1, 잘못된 옵션은 2로 끝납니다. `--seed`와 서버 옵션을 함께 쓰거나 옵션을 중복하면 거절합니다. `--cache off`는 캐시 연결이나 상태 확인을 하지 않습니다. SQLite 연결 하나를 재사용하고 잠금 대기는 최대 1초입니다. SIGTERM이나 SIGINT로 종료합니다.

`GET /products/{id}`는 상품 JSON을 200으로, 없는 상품을 `404 product_not_found`로 반환합니다. id 범위는 1~2,147,483,647이고 가격은 0~1,000,000,000입니다. 이름은 NUL 없는 UTF-8 1~120바이트입니다. 잘못된 id·query·GET body는 `400 invalid_request`, 모르는 경로는 `404 route_not_found`, 지원하지 않는 메서드는 `405 method_not_allowed`와 `Allow: GET`입니다. 원본 조회 실패나 잘못된 원본 데이터는 `503 origin_unavailable`이며, 다음 요청에서 원본을 다시 시도합니다. 모든 응답의 Content-Type은 `application/json`이고 오류는 `{"error":"고정된_영어_문자열"}` 형태입니다.

`GET /products`는 조건에 맞는 상품을 `{"items":[...]}`로 반환합니다. 매개변수는 `q`(부분 문자열, 대소문자 구분, 최대 120바이트), `min_price`·`max_price`(0~1,000,000,000, 양 끝 포함), `sort`(`id`, `price_asc`, `price_desc`), `offset`(0~1,000,000), `limit`(1~100)이며 기본값은 빈 `q`, 0, 1,000,000,000, `id`, 0, 20입니다. 가격이 같으면 id 오름차순입니다. 매개변수 이름과 값을 한 번 디코딩하며, 중복·알 수 없는 이름·깨진 인코딩·범위 밖 값·`min_price > max_price`는 `400 invalid_request`입니다. 정렬된 조건의 고정된 표현으로 캐시 key를 만들어 매개변수 순서와 기본값 표기가 달라도 같은 key를 씁니다. 빈 `items`도 정상 200이므로 저장합니다.

`PUT /products/{id}`는 `{"name":...,"price":...}` 전체로 기존 상품을 수정하고 성공은 204입니다. 부분 수정·신규 생성·추가 필드·중복 필드·누락 필드는 받지 않습니다. JSON이 아닌 Content-Type은 415 `unsupported_media_type`, 4 KiB를 넘는 body는 413 `body_too_large`, 그 밖의 잘못된 JSON·이름·가격은 400 `invalid_request`, 없는 상품은 404 `product_not_found`입니다. `DELETE /products/{id}`는 기존 상품을 삭제하고 성공은 204, 이미 없으면 404입니다. 두 요청 모두 원본 트랜잭션이 commit된 뒤에만 응답하며, 실패하면 rollback하고 캐시를 무효화하지 않습니다.

원본 변경이 성공하면 해당 상세 key를 DELETE하고 목록 세대를 하나 올립니다. 목록·검색 key에 세대가 들어 있으므로 이전 세대의 응답은 다시 읽히지 않고 TTL과 주기 청소로 회수됩니다. 다른 상품의 상세 key는 지우지 않습니다. 이 넓은 무효화는 변경과 무관한 목록·검색 응답도 버리므로 수정이 잦으면 적중률이 떨어집니다. 상세 DELETE가 204나 404 `key_not_found`가 아니면 캐시 사용을 중단하고 수정은 성공으로 응답합니다. 서버는 상품 요청 하나를 원본·캐시 처리까지 끝낸 뒤 다음 요청을 처리하므로 수정과 조회가 중간에 섞이지 않습니다.

`GET /stats`는 이 실행의 `product_gets`(유효한 상품 GET), `origin_queries`(404와 실패를 포함한 원본 조회 시도), `origin_failures`(원본 조회·변경 실패), `origin_changes`(원본 변경 성공), `invalidations`(목록 세대를 올린 논리적 무효화 수), `cache_bypasses`(캐시 미사용 조회), `cache_state`(`off`, `active`, `suspended`)를 반환합니다. 통계 요청과 준비 작업은 원본 조회로 세지 않습니다. 추가 카운터는 `cache_gets`(GET 시도), `cache_hits`(200), `cache_misses`(404), `cache_get_errors`(그 외 상태·통신 오류), `cache_puts`(PUT 시도), `cache_put_failures`(PUT 실패), `cache_deletes`(무효화 DELETE 시도), `cache_delete_failures`(204·404가 아닌 DELETE), `cache_probes`(상태 확인 시도), `cache_probe_failures`(상태 확인 실패), `cache_recoveries`(재개 횟수), `cache_errors`(GET 오류 + PUT 실패 + DELETE 실패)입니다. 적중률 분모는 `cache_gets = cache_hits + cache_misses + cache_get_errors`이며, off 또는 중단 상태의 조회는 `cache_bypasses`로만 셉니다.

정상 200 body만 TTL 헤더를 붙여 저장하며, 적중 시 TTL을 늘리지 않습니다. 실행마다 새 식별자로 이전 캐시와 격리합니다.

캐시 GET·PUT·DELETE의 연결 실패, 기한 초과, 응답 중 연결 종료, 오류 상태는 캐시 오류로 보고 `suspended`로 바꿉니다. 오류 뒤 연결을 닫고, 그 응답은 원본에서 만든 것을 그대로 돌려줍니다. 중단 중 실제 상품 요청이 오면 1초에 한 번만 전용 key GET으로 상태를 확인합니다. 정상적인 404 `key_not_found`를 받으면 새 실행 식별자와 초기 목록 세대로 재개하고, 실패하면 원본만 사용합니다. 이전 식별자의 key는 읽지 않고 TTL과 주기 청소로 회수되므로, 늦게 도착한 이전 PUT이나 재시작 전 값은 새 실행에서 재사용되지 않습니다. 유휴 중 배경 작업은 없습니다. 재현: `./build/mini-redis 8080`을 띄우고 데모를 `--cache on`으로 시작해 `/products/1`을 두 번 조회한 뒤, mini-redis를 끄고 다시 조회하면 `/stats`의 `cache_get_errors`가 늘고 `cache_state`가 `suspended`가 됩니다. mini-redis를 다시 띄우고 1초 뒤 조회하면 `cache_recoveries`가 늘고 첫 조회는 원본을, 다음 조회는 새 key를 사용합니다. 한계: 단일 프로세스만 같은 캐시를 쓰며, 재개 판정은 지연된 이전 PUT의 도착이 아니라 새 식별자로 격리합니다.

### 원본 DB와 캐시 실험

상품 요청 처리(`demo/product_demo.c`)와 원본 접근(`demo/sqlite_origin.c`, 선택 `demo/postgres_origin.c`)을 나눴습니다. 원본은 `id` key와 `(price,id)` 인덱스를 같은 의미로 유지하며, SQLite 연결은 `busy_timeout` 1초로 잠금을 기다립니다. PostgreSQL은 libpq를 쓰는 선택 빌드이고 `statement_timeout`을 1초로 두며 연결을 재사용합니다. `--db` 값이 SQLite 파일 경로이거나 PostgreSQL 연결 문자열(`postgresql://...`)입니다.

```bash
make cache-demo-postgres                      # libpq 개발 파일이 있을 때만
./build/cache-demo-postgres --db 'postgresql://127.0.0.1/products' --seed 1 --count 10000
./build/cache-demo-postgres --db 'postgresql://127.0.0.1/products' --port 8081 --cache off
make cache-functional-postgres POSTGRES_URL='postgresql://127.0.0.1/products'
```

기본 빌드, `make verify`, CI는 libpq나 실행 중인 PostgreSQL을 요구하지 않습니다. `/stats`의 `origin` 값이 `sqlite` 또는 `postgresql`입니다. PostgreSQL 테스트는 임시 DB를 만들지 않으므로 지정한 DB의 `products` 내용을 seed 명령이 교체합니다.

캐시 on/off 비교는 `make cache-bench`가 최적화 빌드로 실행합니다. 기본 입력은 상품 10,000개, 연결 32개, 초당 500요청, 준비 10초, 측정 30초, 3회, TTL 60초와 2초입니다. `--quick`은 준비 1초·측정 3초·1회로 경로만 확인합니다. `--kind detail|list|search`, `--scenario 이름`, `--warmup/--measure/--runs`, `--db`(PostgreSQL 연결 문자열)를 받습니다. 측정은 `make verify`와 CI에 넣지 않습니다. 결과는 `/tmp` 같은 저장소 밖 파일로 남깁니다.

#### 원격 PostgreSQL 비교 (미실행)

다른 컴퓨터의 PostgreSQL은 `make cache-bench-postgres POSTGRES_URL='postgresql://user@host/db'`로 같은 계획을 실행합니다. 이 저장소를 작성한 환경에서는 원격 DB가 제공되지 않아 원격 측정을 실행하지 않았습니다. 로컬 PostgreSQL 결과를 원격 비용으로 해석하지 않습니다.

#### 선택 PostgreSQL 조회 전용 측정 결과

같은 컴퓨터의 PostgreSQL 16.15(Homebrew, 별도 프로세스)에서 위와 같은 계획으로 1회씩 쟀습니다(준비 3초, 측정 10초, TTL 60초). 상세 균등은 p50 4.69→6.88 ms, 적중률 35.9%, 원본 5,024→3,220, 목록 반복은 7.07→2.83 ms, 적중률 100%, 원본 5,024→0, 검색 다양한 조건은 25.23→36.46 ms, 적중률 0%입니다. SQLite와 다른 원본 환경이므로 이 차이를 네트워크 비용 하나로 해석하지 않습니다.

#### SQLite 조회 전용 측정 결과

2026-11-14, macOS 26.6.2 arm64, python 3.15.2, SQLite 3.54.1에서 `-O2`(sanitizer 끔) 빌드로 쟀습니다. 입력은 상품 10,000개 seed 1, 연결 32개, 초당 500요청, 준비 3초, 측정 10초, 2회, TTL 60초이고 `make cache-bench`의 기본값(준비 10초·30초·3회)보다 짧게 잰 값입니다. 명령은 `python3 -B scripts/bench/cache_bench.py build/bench/cache-demo build/bench/mini-redis --warmup 3 --measure 10 --runs 2 --ttl 60 --scenario ...`입니다. 처리량은 두 모드 모두 502.4/s(제안 부하 한도), 오류와 timeout은 0입니다.

| 시나리오 | cache off p50/p95 (ms) | cache on p50/p95 (ms) | 원본 조회 off→on | 적중률 |
|---|---|---|---|---|
| 상세, 균등 | 1.31 / 3.10 | 4.03 / 5.37 | 5,024→3,220 | 35.9% |
| 상세, 20%에 80% 집중 | 1.18 / 3.12 | 3.64 / 5.28 | 5,024→2,356 | 53.1% |
| 목록, 같은 조건 반복 | 10.20 / 14.51 | 2.67 / 3.36 | 5,024→0 | 100% |
| 목록, 다양한 조건 | 13.62 / 20.02 | 16.24 / 20.54 | 5,024→5,024 | 0% |
| 검색, 같은 조건 반복 | 3.32 / 5.20 | 2.91 / 3.48 | 5,024→0 | 100% |
| 검색, 다양한 조건 | 15.60 / 20.71 | 17.96 / 24.95 | 5,024→5,024 | 0% |

같은 조건을 반복하는 목록·검색은 적중률 100%로 원본 조회가 사라지고 p50이 10.2→2.7 ms, 3.3→2.9 ms로 줄었습니다. 반면 단일 스레드 데모가 mini-redis 왕복을 기다리므로 상세 조회는 p50이 1.2→ 3.6~4.0 ms로 늘었고, 매번 조건이 달라지는 목록·검색은 적중이 없어 왕복 비용만 더해졌습니다. 이 수치는 합격 기준이 아니며 장비·부하·데이터가 다르면 달라집니다.

## API

다른 서버는 HTTP로 mini-redis를 부릅니다. 동작은 메서드로, key는 경로 `/keys/<key>`로, value는 body의 바이트 그대로 나타냅니다.

| 요청 | 동작 |
|---|---|
| `PUT /keys/<key>` | body를 value로 저장합니다. 같은 key가 있으면 덮어씁니다. `TTL` 헤더로 보관 기간을 정할 수 있습니다. |
| `GET /keys/<key>` | 저장한 value를 body로 돌려줍니다. |
| `DELETE /keys/<key>` | key를 지웁니다. |

- **key:** `/keys/` 뒤의 나머지 전부입니다. 퍼센트 인코딩(`%HH`)을 정확히 한 번 풀고, 그 바이트 그대로 key로 씁니다. 그래서 `%2F`는 경로 구분자가 아니라 key 안의 `/`이고, `%00`이 든 key도 잘리지 않으며, `%2541`은 `%41`이라는 key가 됩니다. 한글, 공백, `/`가 든 key는 인코딩해서 보냅니다. key 안의 `?`는 반드시 `%3F`로 보냅니다. 인코딩하지 않은 `?`가 있으면 `400 bad_key_encoding`입니다. 그래서 `/keys/a?b=1` 같은 query는 쓸 수 없습니다.
- **value:** body의 바이트 그대로이고, 길이는 `Content-Length`로 알립니다. JSON으로 감싸지 않습니다. `'\0'`이나 UTF-8이 아닌 바이트가 든 value(이미지 등)도 그대로 저장하고 돌려받습니다. 0바이트 value도 저장할 수 있고, 조회하면 `200`과 0바이트 body를 받습니다.
- **보관 기간:** `PUT`에 `TTL: <초>` 헤더를 붙이면 그 key는 그 초 동안만 있습니다. 헤더가 없으면 지울 때까지 남습니다. 헤더 이름은 대소문자를 가리지 않습니다.
  - **받는 값:** 단위는 초이고, 10진수 숫자로만 된 1 이상의 정수만 받습니다. 값 앞뒤의 공백은 받습니다. 0, 음수, `+` 부호, 숫자 사이의 공백, 숫자가 아닌 값, 빈 값, 너무 큰 값(약 1.8×10¹⁶초를 넘는 값), 한 요청에 두 번 온 `TTL` 헤더는 `400 bad_ttl`이고, 저장하지 않습니다. 0을 "바로 지우기"로 받지 않는 것은 client의 실수가 조용히 데이터 삭제가 되지 않게 하기 위해서입니다.
  - **세는 시작:** 서버가 그 요청의 헤더를 다 받은 순간부터 셉니다. 큰 body를 천천히 올려도 올리는 시간만큼 늦게 만료되지 않습니다.
  - **만료:** 보관 기간이 끝나는 순간부터 그 key는 없는 key와 같습니다. `GET`과 `DELETE`는 `404 key_not_found`를 받고, "만료됨"을 따로 알리지 않습니다.
  - **덮어쓰기:** `PUT`은 key의 보관 기간도 새로 정합니다. 기간이 남은 key에 `TTL: 5`로 다시 `PUT`하면 그때부터 5초이고, `TTL` 없이 다시 `PUT`하면 기간이 사라집니다. 기간만 바꾸거나 남은 기간을 묻는 API는 없습니다.
  - **`GET`과 `DELETE`:** `GET`은 보관 기간을 늘리지 않습니다. 두 요청에 붙인 `TTL` 헤더는 무시합니다.
  - **메모리:** 아무도 다시 조회하지 않는 만료된 key도 서버가 25 ms마다 한 번에 2,000개까지 지워 메모리를 돌려받습니다.
- **body 상한:** 16 MiB(16,777,216바이트)입니다. `Content-Length`가 이 값을 넘으면 서버는 body를 받기 전에 `413`을 보내고 연결을 닫습니다.
- **연결:** 서버는 동시 연결을 128개까지 받습니다. 128개가 열려 있을 때 새로 연 연결은 `503`을 받고 닫히며, 열린 연결이 닫혀 자리가 나면 다시 받습니다. 연결 하나로 요청을 여러 번 보낼 수 있습니다.
- **쉬는 시간 기한:** 연결에서 10초 동안 바이트가 오가지 않으면(받기와 보내기 모두) 서버가 연결을 닫습니다. 요청을 보내다 멈춘 연결은 `408`을 받고 닫히고, 응답을 받은 뒤 쉬는 연결과 아무것도 보내지 않은 연결은 응답 없이 닫힙니다. 큰 body를 천천히 보내거나 큰 응답을 천천히 받아 가는 동안에는 바이트가 오가므로 닫히지 않습니다. 반대로 응답을 받다가 10초 동안 전혀 읽지 않으면, 응답 중간이라도 응답 없이 닫힙니다.
- **body 길이:** `Content-Length`로만 알립니다. `Transfer-Encoding: chunked`로 보내면 서버는 body를 받기 전에 `411`을 보내고 연결을 닫습니다. chunked body는 끝까지 받아야 길이를 알 수 있어서 상한을 먼저 검사할 수 없기 때문입니다.

### 응답

| 상황 | 상태 코드 | body |
|---|---|---|
| 조회 성공 | `200` | value 바이트 (`Content-Type: application/octet-stream`) |
| 저장 성공 (새로 만듦, 덮어씀 구분 없음) | `204` | 없음 |
| 삭제 성공 | `204` | 없음 |
| 조회, 삭제할 key가 없음 (만료된 key 포함) | `404` | `{"error":"key_not_found"}` |
| `/keys/`로 시작하지 않는 경로 (`/keys`도 포함) | `404` | `{"error":"route_not_found"}` |
| 0바이트 key (`/keys/`) | `400` | `{"error":"empty_key"}` |
| 깨진 퍼센트 인코딩 (`%G1`, 끝이 잘린 `%2`), 경로에 인코딩하지 않은 `?` | `400` | `{"error":"bad_key_encoding"}` |
| `PUT`의 `TTL`이 1 이상의 정수가 아니거나, 너무 크거나, 두 번 옴 | `400` | `{"error":"bad_ttl"}` |
| `PUT`, `GET`, `DELETE`가 아닌 메서드 | `405` | `{"error":"method_not_allowed"}`, 헤더 `Allow: GET, PUT, DELETE` |
| 요청을 보내다 10초 동안 멈춤 | `408` | `{"error":"request_timeout"}`, 보낸 뒤 연결을 닫음 |
| `Transfer-Encoding: chunked` 요청 | `411` | `{"error":"length_required"}` |
| `Content-Length`가 16 MiB를 넘음 | `413` | `{"error":"body_too_large"}` |
| 서버의 메모리 부족 | `500` | `{"error":"out_of_memory"}` |
| 동시 연결이 128개일 때 새 연결 | `503` | `{"error":"too_many_connections"}`, 보낸 뒤 연결을 닫음 |

오류 응답의 `Content-Type`은 `application/json`입니다. 상태 코드가 같은 오류도 body의 오류 이유로 나눌 수 있습니다.

**오류 이유:** `key_not_found`, `route_not_found`, `empty_key`, `bad_key_encoding`, `bad_ttl`, `method_not_allowed`, `request_timeout`, `length_required`, `body_too_large`, `out_of_memory`, `too_many_connections`.

**오류의 우선순위:** 한 요청에 오류가 여럿이면 처음 걸린 하나로 답합니다.

1. `Transfer-Encoding`이 있으면, 경로와 메서드를 보기 전에 `411`입니다. 헤더가 도착하자마자 검사하기 때문입니다.
2. `Content-Length`가 16 MiB를 넘으면, 같은 이유로 경로와 메서드를 보기 전에 `413`입니다.
3. 경로가 `/keys/`로 시작하지 않으면 `404 route_not_found`입니다.
4. 메서드가 `PUT`, `GET`, `DELETE`가 아니면 `405`입니다.
5. key의 인코딩이 깨졌거나 인코딩하지 않은 `?`가 있으면 `400 bad_key_encoding`, key가 비었으면 `400 empty_key`입니다.
6. `PUT`의 `TTL`이 잘못됐으면 `400 bad_ttl`입니다.

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

보관 기간 1초로 저장하고, 바로 조회한 뒤 1초가 지나 다시 조회합니다.

```console
$ curl -i -X PUT -H "TTL: 1" --data-binary "hello" http://127.0.0.1:8080/keys/greeting
HTTP/1.1 204 No Content

$ curl -i http://127.0.0.1:8080/keys/greeting
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Content-Length: 5

hello
$ sleep 1.2
$ curl -i http://127.0.0.1:8080/keys/greeting
HTTP/1.1 404 Not Found
Content-Type: application/json
Content-Length: 25

{"error":"key_not_found"}
```

`TTL`이 0이면 저장하지 않습니다.

```console
$ curl -i -X PUT -H "TTL: 0" --data-binary "hello" http://127.0.0.1:8080/keys/greeting
HTTP/1.1 400 Bad Request
Content-Type: application/json
Content-Length: 19

{"error":"bad_ttl"}
```

## 파일 트리

```
mini-redis/
├─ Makefile          make verify, make test, make bench
├─ src/              소스 코드
├─ tests/            <마일스톤 이름>_test.c, functional/
├─ scripts/          check-ids.py, convention.py, bench/(지연 측정)
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
| M2 | [`api`](docs/milestones/api/overview.md) | 외부에서 부르기 | L3 L10 L14 | 진행 중 | |
| M3 | [`concurrency`](docs/milestones/concurrency/overview.md) | 동시성 | L8 | 진행 중 | [대조](docs/milestones/concurrency/spec.md#redis와-대조) |
| M4 | [`expire`](docs/milestones/expire/overview.md) | 보관 기간 | L9 | 진행 중 | [대조](docs/milestones/expire/spec.md#redis와-대조) |
| M5 | [`cache`](docs/milestones/cache/spec.md) | API 응답 캐시, 무효화, 성능 비교 | L15 L11 L17 | 진행 중 | [대조](docs/milestones/cache/spec.md#redis와-대조) |
| M6 | `persist` | 서버가 꺼져도 데이터 유지 (선택) | L12 | 진행 전 | |

요구사항 ID는 [`docs/requirements.md`](docs/requirements.md)에 있습니다. L16(엣지 케이스)은 모든 마일스톤의 테스트에 들어갑니다. 마일스톤이 끝나면 `Redis와 대조` 칸에 그 `spec.md`의 `Redis와 대조` 절을 링크합니다.

## 알려진 한계

- **M5의 응답 캐시 데모는 백엔드 한 프로세스로 제한합니다.** 여러 백엔드 프로세스가 같은 원본과 캐시를 공유할 때의 조회·수정 순서 보장은 구현하지 않습니다. 원본 변경은 데모 API를 통해서만 하며, DB나 파일을 직접 수정하는 경우의 캐시 무효화는 보장하지 않습니다.
- **M5는 상품 수정 시 해당 상품의 상세 캐시와 목록·검색 캐시 전체를 무효화하는 방식으로 설계합니다.** 검색 조건별 영향을 추적하지 않으므로, 변경과 무관한 목록·검색 응답도 함께 무효화합니다. 수정이 잦으면 캐시 적중률이 낮아지고 원본 조회와 캐시 재생성이 늘어날 수 있습니다. 다른 상품의 상세 캐시는 유지합니다.
- **M5의 이전 세대 목록·검색 key는 TTL이 끝날 때까지 메모리를 차지합니다.** 세대를 올리면 다시 읽히지 않지만 즉시 지우지 않으므로, 수정이 잦으면 그만큼 쌓입니다. mini-redis에 패턴 삭제를 추가하지 않았습니다.
- **M5 데모는 요청 하나를 원본·캐시 처리까지 끝낸 뒤 다음 요청을 처리합니다.** mini-redis 왕복을 기다리는 동안 다른 상품 요청도 기다리므로, 상세처럼 원본 조회가 싼 조건에서는 캐시가 더 느릴 수 있습니다. 측정값은 README의 "SQLite 조회 전용 측정 결과"에 있습니다.
- **M5는 실제 메모리 부족과 다중 백엔드 경쟁을 검증하지 않았습니다.** 캐시 중단·재개와 원본 잠금 오류는 확인했지만, 할당 실패·메모리 압박·분산 잠금은 시험하지 않았습니다.
- **원격 PostgreSQL 실험은 미실행입니다.** 절차와 명령만 확인했고, 원격 환경이 없어 수치를 만들지 않았습니다.
- eviction(메모리가 가득 찰 때 key를 내보내는 정책)은 LMS에 없어 구현하지 않습니다.
- **key 수가 2배로 늘어나는 순간 서버가 한 번 멈춥니다.** 해시 테이블은 버킷 수를 늘릴 때 entry 전부를 한 번에 옮기고, 그동안 서버는 어떤 요청에도 답하지 않습니다. 버킷은 entry가 버킷의 4배를 넘을 때 2배로 늘어나므로, 이 멈춤은 key 수가 2배가 될 때마다 한 번 생깁니다. store가 약 13만 개를 넘으면 한 번의 멈춤이 M3의 목표(방해 요청이 있어도 작은 GET의 p99 10 ms 이하)를 넘고, 400만 개에서는 약 310~350 ms입니다. 같은 경계에서 저장과 삭제를 반복해도 rehash가 반복되지는 않습니다(전에는 요청마다 300 ms 넘게 멈췄음). 삭제나 주기 청소로 entry 수가 버킷 수 아래로 줄 때도 버킷을 줄이는 rehash가 한 번 일어나고, 남은 entry가 약 13만 개를 넘으면 그 한 번도 목표를 넘습니다. Redis처럼 나눠 옮기면 없앨 수 있지만, Pintos 해시 테이블의 구조를 지키려고 하지 않았습니다(`docs/milestones/concurrency/spec.md`의 "측정 결과").
- **16 MiB value를 저장하는 동안 서버가 약 12 ms 멈춥니다.** 상한 크기의 value를 초당 5번 저장하면 작은 GET의 p99가 10.7~11.0 ms로 목표를 약 1 ms 넘습니다. store가 value를 새 메모리로 복사하는 시간입니다. 줄이려면 body 상한이나 store의 인터페이스를 바꿔야 해서 줄이지 않습니다.
- **컴퓨터가 절전하는 동안에는 보관 기간이 흐르지 않습니다.** 만료는 시스템 시각이 바뀌어도 튀지 않는 단조 시계로 판정하는데, 이 시계는 절전 동안 멈춥니다. 노트북이 1시간 잠들면 key가 1시간 더 남습니다. 서버는 보통 절전하지 않으므로 받아들였습니다(ADR `monotonic-clock-for-expiry`).
- **key는 요청이 서버에 닿는 데 걸린 시간만큼 늦게 만료됩니다.** 보관 기간은 서버가 헤더를 받은 순간부터 세므로, client가 보낸 순간보다 편도 전송 지연만큼 늦게 끝납니다. 일찍 만료되는 일은 없습니다.
- **요청이 없어도 서버가 초당 40번 깨어납니다.** 주기 청소가 25 ms 간격을 지키도록 루프의 대기 시간도 25 ms로 두었기 때문입니다(`docs/milestones/expire/spec.md`의 "측정 결과").
- 바이트를 아주 조금씩 계속 보내거나 받아 가는 연결은 쉬는 시간 기한(10초)에 걸리지 않습니다. 이런 연결이 차지할 수 있는 자리는 연결 상한 128개로 묶입니다.
- 연결 상한에 걸린 연결은 `503`을 보낸 뒤 곧바로 닫힙니다. client가 그 사이에 요청을 보내면(특히 큰 body), 서버가 읽지 않은 데이터를 남긴 채 닫으므로 커널이 연결을 리셋해서 client가 `503` 대신 연결 리셋 오류를 받을 수 있습니다.
- client가 body를 보내다 스스로 연결을 끊으면, 그때까지 받은 body가 value로 저장될 수 있습니다. mongoose가 닫히는 연결의 받다 만 요청을 요청으로 넘기기 때문입니다. 서버가 먼저 닫는 경우(`408`, `411`, `413`, `503`)에는 저장하지 않습니다.
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
