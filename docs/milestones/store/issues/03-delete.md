# 03: 삭제한 key는 더 이상 조회되지 않는다

**Status:** ready-for-agent

**What to build:** 팀원이 key를 삭제하면 그 key는 조회되지 않고, 삭제 결과로 지웠는지를 받습니다. 없는 key의 삭제는 오류가 아니라 "지운 것이 없음"입니다. store는 `hash_delete`로 뺀 entry를 해제합니다. 결정은 `../spec.md`의 Q9를 따릅니다.

**Blocked by:** 02 (마지막 시나리오가 덮어쓰기가 옛 entry를 남기지 않는지 확인하기 때문입니다)

테스트 시나리오(팀 확인 2026-11-13).

- [x] [T4] 있는 key를 삭제하면 지웠다고 알리고, 그 뒤 조회되지 않는다
- [x] 없는 key를 삭제하면 지운 것이 없다고 알린다
- [x] 같은 key를 두 번 삭제하면 두 번째는 지운 것이 없다고 알린다
- [x] 한 key를 삭제해도 다른 key는 그대로 조회된다
- [x] 같은 key에 두 번 저장한 뒤 한 번 삭제하면 조회되지 않는다
- [x] T1~T4를 `docs/requirements.md` L13의 `근거` 칸에 인용하고 `충족`을 갱신한다
- [x] `make verify`가 통과한다

## Comments
