# 02: 삭제, 없는 key, 빈 value, 바이너리 value

**Status:** ready-for-agent

**What to build:** 다른 서버가 `DELETE /keys/<key>`로 key를 지울 수 있고, 없는 key를 조회하거나 지우면 `404`와 오류 이유 `key_not_found`를 받습니다. 0바이트 value와 `'\0'`이나 UTF-8이 아닌 바이트가 든 value도 그대로 저장하고 돌려받습니다. 오류 응답의 body는 `{"error":"<오류 이유>"}`이고 `Content-Type`은 `application/json`입니다. 결정은 `../spec.md`의 Q5, Q8, Q9와 "문제 5 빌린 value"를 따릅니다.

**Blocked by:** 01

테스트 시나리오(팀 확인 2026-11-13). 테스트 이름은 아래 문장을 그대로 씁니다.

- [ ] 없는 key를 조회하면 404와 {"error":"key_not_found"}를 돌려준다
- [ ] 있는 key를 지우면 204를, 없는 key를 지우면 404와 {"error":"key_not_found"}를 돌려준다
- [ ] 0바이트 value를 저장하고 조회하면 200과 0바이트 body를 돌려준다
- [ ] 조회 응답의 Content-Type은 application/octet-stream이고, 오류 응답은 application/json이다
- [ ] (기능) '\0'과 UTF-8이 아닌 바이트가 든 value를 그대로 돌려받는다
- [ ] (리뷰) 서버는 `store_get`이 빌려준 value를 다음 store 호출 전에 mongoose의 송신 버퍼로 복사한다
- [ ] (리뷰) 메모리 부족이면 500과 {"error":"out_of_memory"}를 돌려준다. 할당 함수를 바꿔 끼울 seam이 없어 자동 테스트하지 않습니다
- [ ] `make verify`가 통과한다

## Comments
