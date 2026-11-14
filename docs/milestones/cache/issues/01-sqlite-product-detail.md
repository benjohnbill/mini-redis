# 01: SQLite 원본에서 상품 상세를 조회한다

**What to build:** client가 C와 기존 Mongoose로 만든 별도 데모 백엔드에 상품 상세를 요청하면 SQLite 원본의 JSON 응답을 받습니다. 먼저 캐시 off 기준선을 실제 서버와 DB로 실행하고 외부 HTTP 테스트로 확인합니다.

**Blocked by:** 00 — macOS에서도 기존 검증 빌드를 실행한다.

Status: ready-for-agent

- [ ] 정해진 seed의 상품 데이터와 가격·id 인덱스를 준비하고, 상품 id·이름·가격의 범위를 M5 spec대로 검사한다.
- [ ] GET /products/{id}가 정상 상품은 200, 없는 상품은 404 product_not_found, 잘못된 입력은 400 invalid_request로 반환한다. 미지원 경로·메서드도 명세의 오류 계약을 따른다.
- [ ] 캐시 off 모드에서는 mini-redis에 연결하지 않고, 실행 중 재사용하는 SQLite 연결로 응답을 만든다. 원본 없이 시작하면 실패하고 실행 중 DB 오류는 503 origin_unavailable로 반환한다.
- [ ] GET /stats에서 상품 GET과 원본 조회 시도·실패의 수치를 확인할 수 있다. 상세 조회 하나는 원본 조회 한 번이며 통계 요청은 원본 조회를 늘리지 않는다.
- [ ] 임시 DB와 실제 데모 프로세스를 띄우는 외부 HTTP 테스트로 응답·카운터·원본 오류 후 회복을 확인한다. 내부 helper나 메모리 부족 테스트는 추가하지 않는다.
- [ ] 데모 시작·종료·실패 시 자원 정리를 검증하고, 기능 테스트는 기존 sanitizer 검사 방식을 따른다.
- [ ] make cache-demo와 make cache-functional을 제공하고 SQLite 의존성·실행법·CI 설치를 반영한다. 기본 기능 테스트를 make verify에 포함하되 기존 mini-redis 실행 파일의 DB 의존성은 늘리지 않는다.

## Comments

- M5 spec의 상품 데이터, HTTP 계약, 원본 조회 카운터, 외부 HTTP 테스트 경계를 따른다. 데모 백엔드의 책임 ADR을 유지한다.
- 선행 구조 변경은 필요하지 않다. 기존 mini-redis의 공개 함수나 HTTP 계약을 수정하지 않고 별도 실행 경로로 시작한다.
