# 04: 상품 수정과 삭제 뒤 상세·목록·검색 캐시를 무효화한다

**What to build:** client가 원본 상품을 수정하거나 삭제하면 성공 이후의 상세·목록·검색에서 이전 응답을 받지 않습니다. 다른 상품의 상세 캐시는 유지됩니다.

**Blocked by:** 03 — 상품 목록과 검색 조건별 응답을 캐시한다.

Status: ready-for-agent

- [x] PUT /products/{id}가 name·price 전체 수정, DELETE /products/{id}가 기존 상품 삭제를 제공한다. 성공은 204, 없는 상품은 404이며 상품 생성으로 해석하지 않는다.
- [x] 수정 JSON·필드·숫자·Content-Type·4 KiB body 상한과 원본 오류를 명세대로 처리한다. 실패한 원본 트랜잭션은 롤백하고 유효한 캐시를 무효화하지 않는다.
- [x] 원본 commit 뒤 해당 상세 key를 DELETE하고 목록 세대를 바꾼다. DELETE의 204와 404 key_not_found를 성공으로 다룬다.
- [x] 이전 세대의 목록·검색은 즉시 재사용을 막고 남은 key는 TTL과 기존 주기 청소로 회수한다. 패턴 삭제·전체 key 열거 기능을 mini-redis에 추가하지 않는다.
- [x] 다른 상품의 상세 캐시는 유지한다. 관련 검색 조건을 추적하지 않으므로 변경과 무관한 목록도 무효화한다는 한계를 문서화한다.
- [x] 한 스레드에서 상품 요청 하나의 원본·캐시 처리를 끝낸 뒤 다음 상품 요청을 처리한다. 수정 성공 응답 전에 무효화 또는 캐시 사용 중단을 끝낸다.
- [x] commit 후 상세 무효화가 실패하면 캐시 사용을 중단하고 수정은 성공으로 응답한다. 다음 조회는 원본을 읽으며 원본 수정을 자동 재시도하지 않는다. 복구·재개는 05에서 완성한다.
- [x] 외부 HTTP 테스트로 수정·삭제 뒤 상세, 검색 신규 진입·이탈, 정렬·페이지 변화, 다른 상품 상세의 적중 유지, 실패한 수정 뒤 기존 응답 유지를 확인한다.
- [x] 정상 수정과 삭제, 오류 계약, 무효화·원본 변경 카운터를 외부에서 확인할 수 있고 make verify가 통과한다.

## Comments

- 모든 원본 변경은 한 데모 프로세스의 API를 통과한다. 다중 백엔드 경쟁·외부 직접 수정·메모리 부족 시험은 추가하지 않는다.

## 구현과 검증

- `PUT /products/{id}`와 `DELETE /products/{id}`를 `demo/product_demo.c`에 추가했습니다. 수정은 크기(413), Content-Type(415), JSON과 필드(400)를 캐시와 원본에 접근하기 전에 거절합니다. JSON은 `mg_json_get`으로 body 전체가 하나의 값인지 확인한 뒤 `mg_json_next`로 `name`과 `price`만 중복 없이 받습니다.
- 원본 변경은 `BEGIN IMMEDIATE`·`UPDATE`/`DELETE`·`COMMIT` 한 트랜잭션이고, SQL 오류나 commit 실패는 rollback과 503 origin_unavailable입니다. 성공한 변경만 `origin_changes`를 올리고 `cache_invalidate`를 부릅니다.
- `cache_invalidate`는 목록 세대를 올리고(`invalidations`) 그 상세 key를 `cache_client_delete`로 지웁니다. DELETE의 204와 404 `key_not_found`는 성공이며, 다른 상태나 통신 오류는 `cache_delete_failures`를 올리고 `suspended`로 바꿉니다. 이때도 수정 응답은 204입니다.
- `/stats`에 `origin_changes`, `invalidations`, `cache_deletes`, `cache_delete_failures`를 더했고 `cache_errors`는 GET·PUT·DELETE 실패를 모두 셉니다. 상세 경로의 Allow는 `GET, PUT, DELETE`이고 목록·통계는 `GET`입니다.
- 외부 HTTP 테스트 7개를 `tests/functional/mutation_cache_test.py`에 추가했습니다. 상세·목록 무효화, 검색 진입·이탈과 가격·정렬 변화, 다른 상품 상세 유지, 삭제, 실패한 수정의 보존, 오류 계약, 같은 HTTP 경계 대역으로 commit 뒤 DELETE 500을 재현한 중단을 확인합니다. 기존 `cache_test.py`의 405 Allow 기대값을 새 계약으로 고쳤습니다.
- `PATH=/private/tmp/mini-redis-m5-tools/bin:$PATH make verify` 통과. 재개(중단 해제)는 05에서 구현합니다.
