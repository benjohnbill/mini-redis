# 01: mongoose 서버에서 저장하고 조회하는 한 바퀴

**Status:** ready-for-agent

**What to build:** 다른 서버가 실제 mini-redis 서버에 `PUT /keys/<key>`로 value를 저장하고 `GET /keys/<key>`로 돌려받을 수 있습니다. 서버는 mongoose 위에서 돌고, 요청을 store 호출과 응답으로 바꾸는 일은 api 층이 맡습니다. 이 티켓의 key는 인코딩이 없는 ASCII만 다룹니다. 기능 테스트를 돌리는 틀이 이 티켓에서 생기고, `make verify`와 CI가 그 틀을 실행합니다. 결정은 `../spec.md`의 Q2, Q6, Q7, Q11과 ADR `http-methods-and-raw-body`를 따릅니다.

**Blocked by:** None (can start immediately)

작업에 포함되는 것:

- mongoose 7.23(`mongoose.c`, `mongoose.h`)을 레포에 넣습니다. 출처와 버전은 acutest처럼 빌드 설정의 주석에 적고, 레포 루트에 GPLv2 전문을 담은 LICENSE를 둡니다.
- mongoose는 서버에만 링크하고, api 층의 테스트에는 링크하지 않습니다. `-D_DEFAULT_SOURCE`와 `MG_MAX_RECV_SIZE` 같은 mongoose 전용 옵션은 `mongoose.c`에만 줍니다. `mongoose.h`를 include하는 파일은 이 옵션 없이도 빌드됩니다(4단계에서 확인).
- mongoose 파일은 포맷과 이름 검사에서 뺍니다. 에디터와 `make lint`의 clang-tidy가 읽는 `compile_flags.txt`에 mongoose의 include 경로를 더합니다. 빠지면 서버를 검사할 때 `mongoose.h`를 찾지 못해 `make lint`가 실패합니다.
- 서버는 포트를 첫 인자로 받습니다. SIGTERM이나 SIGINT를 받으면 루프를 빠져나와 mongoose와 store를 해제하고 0으로 끝납니다. 이렇게 해야 서버 프로세스의 AddressSanitizer 오류와 누수가 테스트 실패로 드러납니다.
- 응답 body는 `mg_send`로 보냅니다. `mg_http_reply`는 printf 형식이라 `'\0'`에서 value가 잘립니다.
- 기능 테스트의 틀은 CI에 새 의존성을 들이지 않습니다. 빈 포트를 골라 서버를 띄우고, `sleep` 대신 연결될 때까지 기다리고, 테스트가 실패해도 서버를 끄고, 서버의 종료 코드를 확인합니다. `scripts/check-ids.py`가 `tests/` 아래의 모든 파일을 UTF-8 텍스트로 읽으므로, `tests/` 아래에 바이너리 파일이나 `.pyc`를 만들지 않습니다.

테스트 시나리오(팀 확인 2026-11-13). 테스트 이름은 아래 문장을 그대로 씁니다. "(기능)"은 실제 서버에 요청을 보내는 기능 테스트입니다.

- [ ] 저장한 key를 조회하면 200과 저장한 value를 돌려준다
- [ ] 저장에 성공하면 새로 만들었든 덮어썼든 204를 돌려준다
- [ ] (기능) 실제 서버에 저장한 value를 조회로 돌려받는다
- [ ] (기능) 서버를 끄면 sanitizer 오류 없이 0으로 끝난다
- [ ] `make verify`가 통과한다

## Comments
