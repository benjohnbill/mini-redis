# 02: 상품 상세 응답을 캐시하고 만료 뒤 원본을 다시 읽는다

**What to build:** 같은 상품을 다시 조회하면 mini-redis에 저장한 응답을 사용합니다. 캐시 on/off의 결과는 같고, 원본 조회 횟수는 적중 여부에 따라 달라집니다.

**Blocked by:** 01 — SQLite 원본에서 상품 상세를 조회한다.

Status: ready-for-agent

- [x] 실제 mini-redis의 GET·PUT과 TTL 헤더로 상품 상세 JSON body 전체를 저장·조회한다. 별도 객체 자료형이나 직접 store 호출은 추가하지 않는다.
- [x] 캐시 적중 응답은 원본 조회 없이 application/json으로 반환하고, 미적중은 원본 조회 후 정상 200만 저장한다. 404·원본 오류는 저장하지 않는다.
- [x] 기본 보관 기간 60초와 기간 실행 옵션을 제공한다. 적중 시 TTL은 갱신하지 않으며 모든 저장에는 TTL을 명시한다.
- [x] 캐시 on/off, URL, 요청 기한 입력을 제공한다. 기본 기한 100 ms는 연결부터 응답 완료까지 적용하고 연결을 재사용한다.
- [x] 실행마다 새 식별자로 데모의 캐시를 격리한다. 다른 실행이나 다른 용도의 key를 읽거나 덮어쓰지 않는다.
- [x] 연결·GET·PUT 실패 시 원본에서 만든 응답을 반환하고 캐시 사용을 중단한다. 중단 상태에서 재개하는 동작은 05에서 확장한다.
- [x] GET /stats에서 적중·미적중·오류·미사용 조회·PUT 시도와 실패를 구분한다. 캐시 off는 연결과 상태 확인까지 생략한다.
- [x] 외부 HTTP 테스트로 첫 조회의 원본 접근, 다음 조회의 적중, 서로 다른 상품의 분리, 짧은 TTL 뒤 재조회, 적중에 의한 TTL 연장 없음, 404 미저장을 확인한다.
- [x] 실제 mini-redis 중단 상태에서도 원본 응답을 받고, 데모 재시작 뒤 이전 실행의 캐시를 재사용하지 않는지 확인한다. make verify가 통과한다.

## Comments

- 내부 key 문자열 대신 응답과 카운터 변화를 검사한다. 정확한 만료 순간은 기존 M4 테스트에 맡기고 HTTP 테스트에는 충분한 시간 여유를 둔다.


## 구현과 검증

- `demo/cache_client.c`는 별도 Mongoose manager와 연결을 재사용합니다. 연결부터 응답 완료까지 요청별 기한을 적용하며, 오류 뒤에는 재개 없이 중단합니다. 원본 오류와 404는 저장하지 않습니다.
- 실행 입력은 `--cache on|off`, `--cache-url`, `--ttl-seconds`(기본 60), `--cache-timeout-ms`(기본 100)입니다. URL 기본값은 `http://127.0.0.1:8000`이며 HTTP hostname/IPv4만 지원합니다.
- `/stats`의 GET 시도는 `cache_gets = cache_hits + cache_misses + cache_get_errors`입니다. `cache_puts`, `cache_put_failures`, `cache_errors`(GET 오류와 PUT 실패 합), `cache_bypasses`, `cache_state`(`off`/`active`/`suspended`)를 구분합니다. off와 중단 조회는 GET 시도에 섞지 않습니다.
- TDD 첫 상세 캐시 테스트가 캐시 on 미지원 종료 코드 2로 실패한 뒤 구현하여 통과했습니다. 외부 기능 테스트 9개로 실제 mini-redis GET/PUT, 상품 분리, 404·원본 오류 미저장, 2초 TTL과 적중 비연장, 데모 종료 후 새 실행, 실제 mini-redis 중단, off 연결 생략, CLI 경계를 확인했습니다. 기존 데모 기능 테스트 8개도 유지했습니다.
- `make verify`에서 기존 C·외부 HTTP 테스트, 데모·상세 캐시 기능 테스트, ID 검사와 lint가 통과했습니다. 메모리 부족, 재개, PUT만 실패시키는 fault 주입은 이 티켓에서 검사하지 않았습니다. 장애 재개는 05에서 확장합니다.


### 리뷰 보완 검증 (2026-11-14)

- 404 응답의 `error`가 `key_not_found`일 때만 미적중으로 셉니다. `route_not_found`와 HTML 404는 GET 오류로 세고 캐시를 중단하며 PUT을 생략합니다. 요청 기한이 지나면 중단 처리에서 client manager를 즉시 poll하여 TCP 연결과 송수신 버퍼를 해제합니다. 이벤트 콜백이 반환한 뒤 별도 client manager만 poll하므로 상품 요청에 재진입하지 않습니다.
- 상품 상세 처리에서 원본 조회·검증·JSON 생성과 정상 응답 전송을 분리했습니다. SQLite statement 해제와 원본 실패 집계는 유지했습니다. 양의 정수 옵션 검사를 공유하고 cache helper 정의를 첫 호출 순서에 맞췄습니다.
- 허용된 외부 HTTP·공개 통계 경계에 회귀 시나리오 2개를 추가했습니다. 잘못된 404가 이전에는 미적중 1회와 PUT 1회로 집계되는 RED, 기한 초과 뒤 실행 중인 데모가 상대 TCP 연결을 닫지 않는 RED를 각각 확인한 뒤 수정하여 GREEN을 확인했습니다.
- `PATH=/private/tmp/mini-redis-m5-tools/bin:$PATH make verify` 통과: 기존 테스트 108개, 상품 원본 8개, 상세 캐시 11개로 합계 127개입니다. ASan/UBSan, ID 17개 검사, lint도 통과했습니다. localhost HTTP는 sandbox 밖에서 실행했습니다. 자동 재개와 03 이후 기능, 메모리 부족 검증은 추가하지 않았습니다.
