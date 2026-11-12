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

gcc(또는 clang), make, python3가 필요합니다.

```bash
make verify              # 빌드, 테스트, ID 검사 (CI와 같은 명령)
make test                # 테스트만 실행
./build/mini-redis 8080  # 서버 실행 (포트를 생략하면 8080)
curl http://127.0.0.1:8080/
```

지금 서버는 요청을 하나씩 처리하고, 어떤 요청에도 `ok`를 돌려줍니다.

## 알려진 한계

- eviction(메모리가 가득 찰 때 key를 내보내는 정책)은 LMS에 없어 구현하지 않습니다.
