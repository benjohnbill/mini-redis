# 02: 같은 key에 다시 저장하면 나중 value만 남는다

**Status:** ready-for-agent

**What to build:** 팀원이 이미 있는 key에 다시 저장하면, 조회했을 때 나중 value와 그 길이만 돌아옵니다. store는 새 entry를 먼저 다 만든 뒤 `hash_replace`로 넣고, 밀려난 옛 entry를 통째로 해제합니다. 메모리 할당이 실패하면 테이블을 건드리지 않고 실패를 알립니다. 결정은 `../spec.md`의 Q8을 따릅니다.

**Blocked by:** 01

테스트 시나리오(팀 확인 2026-11-13).

- [x] [T3] 같은 key에 두 번 저장하면 나중 value를 돌려준다
- [x] 길이가 다른 value로 덮어써도 새 길이를 돌려준다
- [x] 덮어쓴 뒤 AddressSanitizer가 옛 entry의 누수를 보고하지 않는다
- [x] 새 entry를 다 만든 뒤에 테이블을 바꾸는 순서를 리뷰에서 확인한다(할당 실패는 자동 테스트하지 않음)
- [x] `make verify`가 통과한다

## Comments
