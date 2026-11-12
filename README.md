# mini-redis

Krafton Jungle Week 11-12: a key-value store on a hash table, usable through an API

## 마일스톤

| 순서 | 이름 | 주제 | 요구사항 | 상태 |
|---|---|---|---|---|
| M0 | — | 준비: Pintos `hash.c` 이식, 요청을 하나씩 받는 서버, 테스트 도구, CI | — | 진행 전 |
| M1 | [`store`](docs/milestones/store/overview.md) | 저장, 조회, 삭제 | L2 L4 L13 | 진행 전 |
| M2 | `api` | 외부에서 부르기 | L3 L10 L14 | 진행 전 |
| M3 | `concurrency` | 동시성 | L8 | 진행 전 |
| M4 | `expire` | 보관 기간 | L9 | 진행 전 |
| M5 | `cache` | API 응답 캐시, 무효화, 성능 비교 | L15 L11 L17 | 진행 전 |
| M6 | `persist` | 서버가 꺼져도 데이터 유지 (선택) | L12 | 진행 전 |

요구사항 ID는 [`docs/requirements.md`](docs/requirements.md)에 있습니다. L16(엣지 케이스)은 모든 마일스톤의 테스트에 들어갑니다.

## 우리 결정과 Redis

마일스톤이 끝날 때마다 그 `spec.md`의 `Redis와 대조` 절을 여기에 링크합니다.

## 실행

M0에서 적습니다.

## 알려진 한계

- eviction(메모리가 가득 찰 때 key를 내보내는 정책)은 LMS에 없어 구현하지 않습니다.
