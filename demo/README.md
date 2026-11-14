# 상품 데모 (M5 `cache`)

mini-redis를 응답 캐시로 쓰는 상품 API 백엔드입니다. 측정 결과는 [`docs/milestones/cache/spec.md`의 "측정 결과"](../docs/milestones/cache/spec.md#측정-결과)에 있습니다.

## 실행

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

## 옵션

준비 옵션은 `--db PATH --seed N [--count N]`입니다. seed는 1~2,147,483,646이며 count는 1~2,147,483,647, 생략하면 10,000입니다. id는 1부터 순서대로, 이름은 `상품 <id>`로 만듭니다. 가격은 seed를 시작값으로 매 상품마다 `state = state * 48271 % 2147483647`, `price = state % 1000000001`로 정합니다. seed 1의 첫 상품은 `{"id":1,"name":"상품 1","price":48271}`입니다. 상품 테이블의 기본 key는 id이고 `(price,id)` 인덱스를 둡니다.

서버 필수 옵션은 `--db PATH --port N --cache on|off`입니다. `--cache-url`은 HTTP 호스트와 선택 port만 받으며 기본값은 `http://127.0.0.1:8000`입니다. IPv6 URL은 지원하지 않습니다. `--ttl-seconds`는 기본 60초, `--cache-timeout-ms`는 기본 100ms이고 둘 다 양의 정수입니다. 캐시 기한은 연결부터 응답 완료까지 요청마다 적용합니다. 캐시 연결과 별도 Mongoose manager를 재사용하여 상품 요청 처리가 재진입하지 않도록 합니다. port는 1~65,535이고 localhost에서만 연결을 받습니다. 준비하지 않은 DB로 시작하면 종료 코드 1, 잘못된 옵션은 2로 끝납니다. `--seed`와 서버 옵션을 함께 쓰거나 옵션을 중복하면 거절합니다. `--cache off`는 캐시 연결이나 상태 확인을 하지 않습니다. SQLite 연결 하나를 재사용하고 잠금 대기는 최대 1초입니다. SIGTERM이나 SIGINT로 종료합니다.

## HTTP API

`GET /products/{id}`는 상품 JSON을 200으로, 없는 상품을 `404 product_not_found`로 반환합니다. id 범위는 1~2,147,483,647이고 가격은 0~1,000,000,000입니다. 이름은 NUL 없는 UTF-8 1~120바이트입니다. 잘못된 id·query·GET body는 `400 invalid_request`, 모르는 경로는 `404 route_not_found`, 지원하지 않는 메서드는 `405 method_not_allowed`와 `Allow: GET`입니다. 원본 조회 실패나 잘못된 원본 데이터는 `503 origin_unavailable`이며, 다음 요청에서 원본을 다시 시도합니다. 모든 응답의 Content-Type은 `application/json`이고 오류는 `{"error":"고정된_영어_문자열"}` 형태입니다.

`GET /products`는 조건에 맞는 상품을 `{"items":[...]}`로 반환합니다. 매개변수는 `q`(부분 문자열, 대소문자 구분, 최대 120바이트), `min_price`·`max_price`(0~1,000,000,000, 양 끝 포함), `sort`(`id`, `price_asc`, `price_desc`), `offset`(0~1,000,000), `limit`(1~100)이며 기본값은 빈 `q`, 0, 1,000,000,000, `id`, 0, 20입니다. 가격이 같으면 id 오름차순입니다. 매개변수 이름과 값을 한 번 디코딩하며, 중복·알 수 없는 이름·깨진 인코딩·범위 밖 값·`min_price > max_price`는 `400 invalid_request`입니다. 정렬된 조건의 고정된 표현으로 캐시 key를 만들어 매개변수 순서와 기본값 표기가 달라도 같은 key를 씁니다. 빈 `items`도 정상 200이므로 저장합니다.

`PUT /products/{id}`는 `{"name":...,"price":...}` 전체로 기존 상품을 수정하고 성공은 204입니다. 부분 수정·신규 생성·추가 필드·중복 필드·누락 필드는 받지 않습니다. JSON이 아닌 Content-Type은 415 `unsupported_media_type`, 4 KiB를 넘는 body는 413 `body_too_large`, 그 밖의 잘못된 JSON·이름·가격은 400 `invalid_request`, 없는 상품은 404 `product_not_found`입니다. `DELETE /products/{id}`는 기존 상품을 삭제하고 성공은 204, 이미 없으면 404입니다. 두 요청 모두 원본 트랜잭션이 commit된 뒤에만 응답하며, 실패하면 rollback하고 캐시를 무효화하지 않습니다.

## 무효화

원본 변경이 성공하면 해당 상세 key를 DELETE하고 목록 세대를 하나 올립니다. 목록·검색 key에 세대가 들어 있으므로 이전 세대의 응답은 다시 읽히지 않고 TTL과 주기 청소로 회수됩니다. 다른 상품의 상세 key는 지우지 않습니다. 이 넓은 무효화는 변경과 무관한 목록·검색 응답도 버리므로 수정이 잦으면 적중률이 떨어집니다. 상세 DELETE가 204나 404 `key_not_found`가 아니면 캐시 사용을 중단하고 수정은 성공으로 응답합니다. 서버는 상품 요청 하나를 원본·캐시 처리까지 끝낸 뒤 다음 요청을 처리하므로 수정과 조회가 중간에 섞이지 않습니다.

## `/stats`

`GET /stats`는 이 실행의 `product_gets`(유효한 상품 GET), `origin_queries`(404와 실패를 포함한 원본 조회 시도), `origin_failures`(원본 조회·변경 실패), `origin_changes`(원본 변경 성공), `invalidations`(목록 세대를 올린 논리적 무효화 수), `cache_bypasses`(캐시 미사용 조회), `cache_state`(`off`, `active`, `suspended`)를 반환합니다. 통계 요청과 준비 작업은 원본 조회로 세지 않습니다. 추가 카운터는 `cache_gets`(GET 시도), `cache_hits`(200), `cache_misses`(404), `cache_get_errors`(그 외 상태·통신 오류), `cache_puts`(PUT 시도), `cache_put_failures`(PUT 실패), `cache_deletes`(무효화 DELETE 시도), `cache_delete_failures`(204·404가 아닌 DELETE), `cache_probes`(상태 확인 시도), `cache_probe_failures`(상태 확인 실패), `cache_recoveries`(재개 횟수), `cache_errors`(GET 오류 + PUT 실패 + DELETE 실패)입니다. 적중률 분모는 `cache_gets = cache_hits + cache_misses + cache_get_errors`이며, off 또는 중단 상태의 조회는 `cache_bypasses`로만 셉니다.

## 저장과 캐시 장애

정상 200 body만 TTL 헤더를 붙여 저장하며, 적중 시 TTL을 늘리지 않습니다. 실행마다 새 식별자로 이전 캐시와 격리합니다.

캐시 GET·PUT·DELETE의 연결 실패, 기한 초과, 응답 중 연결 종료, 오류 상태는 캐시 오류로 보고 `suspended`로 바꿉니다. 오류 뒤 연결을 닫고, 그 응답은 원본에서 만든 것을 그대로 돌려줍니다. 중단 중 실제 상품 요청이 오면 1초에 한 번만 전용 key GET으로 상태를 확인합니다. 정상적인 404 `key_not_found`를 받으면 새 실행 식별자와 초기 목록 세대로 재개하고, 실패하면 원본만 사용합니다. 이전 식별자의 key는 읽지 않고 TTL과 주기 청소로 회수되므로, 늦게 도착한 이전 PUT이나 재시작 전 값은 새 실행에서 재사용되지 않습니다. 유휴 중 배경 작업은 없습니다. 재현: `./build/mini-redis 8080`을 띄우고 데모를 `--cache on`으로 시작해 `/products/1`을 두 번 조회한 뒤, mini-redis를 끄고 다시 조회하면 `/stats`의 `cache_get_errors`가 늘고 `cache_state`가 `suspended`가 됩니다. mini-redis를 다시 띄우고 1초 뒤 조회하면 `cache_recoveries`가 늘고 첫 조회는 원본을, 다음 조회는 새 key를 사용합니다. 한계: 단일 프로세스만 같은 캐시를 쓰며, 재개 판정은 지연된 이전 PUT의 도착이 아니라 새 식별자로 격리합니다.

## 원본 DB

상품 요청 처리(`demo/product_demo.c`)와 원본 접근(`demo/sqlite_origin.c`, 선택 `demo/postgres_origin.c`)을 나눴습니다. 원본은 `id` key와 `(price,id)` 인덱스를 같은 의미로 유지하며, SQLite 연결은 `busy_timeout` 1초로 잠금을 기다립니다. PostgreSQL은 libpq를 쓰는 선택 빌드이고 `statement_timeout`을 1초로 두며 연결을 재사용합니다. `--db` 값이 SQLite 파일 경로이거나 PostgreSQL 연결 문자열(`postgresql://...`)입니다.

```bash
make cache-demo-postgres                      # libpq 개발 파일이 있을 때만
./build/cache-demo-postgres --db 'postgresql://127.0.0.1/products' --seed 1 --count 10000
./build/cache-demo-postgres --db 'postgresql://127.0.0.1/products' --port 8081 --cache off
make cache-functional-postgres POSTGRES_URL='postgresql://127.0.0.1/products'
```

PostgreSQL 연결이나 질의가 실패하면 상세·목록은 503 `origin_unavailable`, 수정·삭제는 rollback 뒤 503이며, 시작할 때 원본을 열 수 없으면 종료 코드 1입니다. 기본 빌드, `make verify`, CI는 libpq나 실행 중인 PostgreSQL을 요구하지 않습니다. `/stats`의 `origin` 값이 `sqlite` 또는 `postgresql`입니다. PostgreSQL 테스트는 임시 DB를 만들지 않으므로 지정한 DB의 `products` 내용을 seed 명령이 교체합니다.

## 성능 비교

캐시 on/off 비교는 `make cache-bench`가 최적화 빌드로 실행합니다. 기본 입력은 상품 10,000개, 연결 32개, 초당 500요청, 준비 10초, 측정 30초, 3회, TTL 60초와 2초입니다. `--quick`은 준비 1초·측정 3초·1회로 경로만 확인합니다. `--kind detail|list|search`, `--scenario 이름`, `--warmup/--measure/--runs`, `--db`(PostgreSQL 연결 문자열)를 받습니다. 측정은 `make verify`와 CI에 넣지 않습니다. 결과는 `/tmp` 같은 저장소 밖 파일로 남깁니다.

## 한계

- **M5의 응답 캐시 데모는 백엔드 한 프로세스로 제한합니다.** 여러 백엔드 프로세스가 같은 원본과 캐시를 공유할 때의 조회·수정 순서 보장은 구현하지 않습니다. 원본 변경은 데모 API를 통해서만 하며, DB나 파일을 직접 수정하는 경우의 캐시 무효화는 보장하지 않습니다.
- **M5는 상품 수정 시 해당 상품의 상세 캐시와 목록·검색 캐시 전체를 무효화하는 방식으로 설계합니다.** 검색 조건별 영향을 추적하지 않으므로, 변경과 무관한 목록·검색 응답도 함께 무효화합니다. 수정이 잦으면 캐시 적중률이 낮아지고 원본 조회와 캐시 재생성이 늘어날 수 있습니다. 다른 상품의 상세 캐시는 유지합니다.
- **M5의 이전 세대 목록·검색 key는 TTL이 끝날 때까지 메모리를 차지합니다.** 세대를 올리면 다시 읽히지 않지만 즉시 지우지 않으므로, 수정이 잦으면 그만큼 쌓입니다. mini-redis에 패턴 삭제를 추가하지 않았습니다.
- **M5 데모는 요청 하나를 원본·캐시 처리까지 끝낸 뒤 다음 요청을 처리합니다.** mini-redis 왕복을 기다리는 동안 다른 상품 요청도 기다리므로, 상세처럼 원본 조회가 싼 조건에서는 캐시가 더 느릴 수 있습니다. 측정값은 `docs/milestones/cache/spec.md`의 "측정 결과"에 있습니다.
- **M5는 실제 메모리 부족과 다중 백엔드 경쟁을 검증하지 않았습니다.** 캐시 중단·재개와 원본 잠금 오류는 확인했지만, 할당 실패·메모리 압박·분산 잠금은 시험하지 않았습니다.
