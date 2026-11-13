# 01: 저장한 key를 조회할 수 있다

**Status:** ready-for-agent

**What to build:** 팀원이 store를 만들고, key와 value를 주소와 길이 한 쌍으로 저장하고, 같은 key로 조회해 value와 길이를 받고, store를 없앨 수 있습니다. entry, hash 함수, less 함수가 이 티켓에서 생깁니다. 결정은 `../spec.md`의 Q1, Q2, Q5, Q6, Q7, Q10, Q11과 ADR `key-value-as-bytes-with-length`, `get-lends-value-until-next-change`를 따릅니다.

**Blocked by:** None (can start immediately)

테스트 시나리오(팀 확인 2026-11-13). 테스트 이름은 아래 문장을 그대로 씁니다.

- [ ] [T1] 저장한 key를 조회하면 저장한 value와 길이를 돌려준다
- [ ] [T2] 없는 key를 조회하면 찾지 못했다고 알린다
- [ ] 0바이트 value를 저장하면 찾았고 길이가 0이라고 알린다
- [ ] 0바이트 key도 저장하고 조회할 수 있다
- [ ] 중간에 '\0'이 있는 value를 잘리지 않고 돌려준다
- [ ] 저장한 뒤 호출한 쪽 버퍼를 바꿔도 저장된 value는 그대로다
- [ ] 앞부분이 같고 길이가 다른 key를 서로 다른 key로 다룬다
- [ ] key를 수천 개 저장한 뒤에도 모든 key를 조회할 수 있다
- [ ] `store_destroy` 뒤에 AddressSanitizer가 누수를 보고하지 않는다
- [ ] `make verify`가 통과한다

## Comments
