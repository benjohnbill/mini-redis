# 03: 상품 목록과 검색 조건별 응답을 캐시한다

**What to build:** client가 상품 목록·이름 검색·가격 필터·정렬·페이지를 요청하고, 같은 의미의 조건은 캐시를 재사용하며 다른 조건의 응답은 섞이지 않습니다.

**Blocked by:** 02 — 상품 상세 응답을 캐시하고 만료 뒤 원본을 다시 읽는다.

Status: ready-for-agent

- [x] GET /products가 q·min_price·max_price·sort·offset·limit를 명세의 기본값과 범위로 처리하고 items 배열을 반환한다.
- [x] 검색은 대소문자를 구분하는 부분 문자열이며 가격 양 끝은 포함한다. 가격이 같으면 id 오름차순으로 정렬한다.
- [x] 깨진 인코딩·중복·알 수 없는 매개변수·잘못된 숫자나 범위를 원본과 캐시에 접근하기 전에 거절한다.
- [x] 검증한 조건의 고정된 표현으로 key를 구성한다. 매개변수 순서와 기본값의 명시 여부가 달라도 같은 의미이면 캐시를 재사용한다.
- [x] 목록·검색 JSON body 전체를 저장한다. 빈 items 배열은 정상 200이므로 캐시하며 오류 응답은 저장하지 않는다.
- [x] 목록·검색 key에 프로세스가 관리하는 목록 세대를 포함한다. 이 티켓에서는 원본 변경 API를 추가하지 않으며 세대 변경은 04가 사용한다.
- [x] 목록·검색의 원본 조회 하나는 SQL 조회 한 번으로 집계하고 별도의 합계 count 조회는 추가하지 않는다.
- [x] 외부 HTTP 테스트로 조건별 응답·분리·동등 조건의 적중·UTF-8 검색·빈 목록·정렬 동률·페이지·입력 오류·TTL 뒤 재조회를 확인한다. 캐시 on/off 응답이 같고 make verify가 통과한다.

## Comments

- 상품 인기 순위·상품 생성·사용자별 응답은 추가하지 않는다. 목록을 상품 ID와 상세 캐시로 분해하는 최적화도 범위 밖이다.

## 구현과 검증

- `GET /products`를 `demo/product_demo.c`에 추가했습니다. 매개변수 이름과 값을 `mg_url_decode`로 한 번 디코딩하고, 중복·알 수 없는 이름·깨진 인코딩·범위 밖 값은 원본과 캐시에 접근하기 전에 400으로 거절합니다. SQL은 `price BETWEEN ?1 AND ?2`, 필요할 때만 `instr(name,?5)>0`, 정렬은 `id`/`price,id`/`price DESC,id`, `LIMIT ?3 OFFSET ?4`로 한 번에 실행합니다.
- 캐시 key는 `list:<목록 세대>:<퍼센트 인코딩한 q>:<min>:<max>:<sort>:<offset>:<limit>`입니다. 검증한 값만 넣으므로 매개변수 순서와 기본값 표기가 달라도 같은 key가 됩니다. 목록 세대는 0으로 시작하며 04가 원본 변경 때 올립니다.
- 목록 하나는 SQL 조회 한 번이고 별도 count 조회가 없습니다. 행마다 상세와 같은 원본 검사를 다시 하며, 잘못된 행이나 SQL 오류는 503 origin_unavailable입니다. 빈 `items`는 저장합니다.
- 외부 HTTP 테스트 9개를 `tests/functional/list_cache_test.py`에 추가했습니다. 기본값·재사용, 동등 조건 key, 조건 분리, UTF-8 검색과 대소문자, 정렬 동률과 페이지, 빈 목록, 잘못된 입력, cache on/off 동일 응답, TTL 2초 뒤 원본 재조회를 확인합니다. `make cache-functional`에 넣었습니다.
- `PATH=/private/tmp/mini-redis-m5-tools/bin:$PATH make verify` 통과: 기존 C·외부 HTTP·데모·상세 캐시 테스트와 목록 테스트 9개, ID 검사, lint. localhost 연결은 sandbox 밖에서 실행했습니다.
