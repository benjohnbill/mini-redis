# M1 `store`: 저장, 조회, 삭제

## 요구사항

L2, L4, L13 (`docs/requirements.md`)

## 앞 마일스톤

M0: Pintos `hash.c`와 `list.c`가 이 레포에서 빌드되고, 빈 테스트 하나가 통과합니다.

## 탐구할 문제

다른 서버가 문자열 key로 문자열 value를 넣고 꺼내려 합니다.

1. key 하나를 평균적으로 상수 시간에 찾으려면, `hash.c`에 무엇을 넘겨야 하나요? (`hash_hash_func`, `hash_less_func`가 하는 일)
2. 같은 key에 두 번 넣으면 값이 하나만 남아야 합니다. `hash_insert`와 `hash_replace` 중 무엇을 쓰고, 옛 값은 어떻게 되나요?
3. `struct hash_elem`은 우리 구조체 안에 들어갑니다. key와 value의 메모리는 누가 할당하고, 삭제할 때 누가 해제하나요?
4. 없는 key를 조회하거나 삭제하면 무엇을 돌려줘야 하나요?
5. key가 수백만 개로 늘면 `hash.c` 안에서 무엇이 일어나고, 그동안 다른 요청은 어떻게 되나요?

## 끝나면 생기는 것

- `spec.md`
- ADR
- `tests/store_test.c`
