# 04: body가 상한을 넘으면 413으로 답한다

**Status:** ready-for-agent

**What to build:** 다른 서버가 상한 16 MiB(16,777,216바이트)를 넘는 body를 보내려 하면, 연결이 이유 없이 끊기는 대신 `413`과 `{"error":"body_too_large"}`를 받습니다. 서버는 헤더가 도착한 이벤트에서 `Content-Length`를 api 층에 넘기고, api 층이 만든 `413` 응답을 body를 받기 전에 보낸 뒤 연결을 닫습니다. mongoose의 `MG_MAX_RECV_SIZE`는 16 MiB보다 크게 올리고, 여유분은 바이트 수로 적습니다. 여유분은 헤더 크기와 mongoose가 수신 버퍼를 늘리는 단위(`MG_IO_SIZE`)를 덮어야 합니다. 아주 긴 key와 상한 크기의 body가 함께 오면 mongoose가 응답 없이 연결을 끊을 수 있다는 점은 받아들입니다. 결정은 `../spec.md`의 Q10을 따릅니다.

**Blocked by:** 03

테스트 시나리오(팀 확인 2026-11-13). 테스트 이름은 아래 문장을 그대로 씁니다.

- [ ] Content-Length가 상한보다 1바이트 크면 413과 {"error":"body_too_large"}를 돌려준다
- [ ] Content-Length가 상한과 같으면 받아들인다
- [ ] (기능) 헤더만 보내도 Content-Length가 상한을 넘으면 413을 받는다
- [ ] (기능) 짧은 key에 상한과 같은 크기의 body를 저장하고 그대로 돌려받는다
- [ ] `make verify`가 통과한다

기능 테스트는 상한을 넘는 요청에서 body를 보내지 않고 헤더만 보낸 뒤 응답을 읽습니다. body까지 보내면, 서버가 읽지 않은 데이터를 남긴 채 연결을 닫을 때 커널이 연결을 리셋해서 클라이언트가 `413`을 받지 못할 수 있습니다.

## Comments
