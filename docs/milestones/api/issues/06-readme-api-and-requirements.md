# 06: README의 "API" 절과 요구사항 기록

**Status:** ready-for-agent

**What to build:** 다른 서버의 개발자가 코드를 읽지 않고 `README.md`의 "API" 절만 보고 mini-redis를 쓸 수 있습니다(L10). 절에는 메서드와 경로, 응답 표, 오류 이유 목록, 저장, 조회, 삭제의 `curl` 예시를 담습니다. `docs/requirements.md`의 L3, L10, L14에 `근거`와 `충족`을 적고, 그 칸이 인용하는 테스트에 `T5`부터 ID를 붙입니다. 결정은 `../spec.md`의 Q13과 Testing Decisions의 "ID"를 따릅니다.

**Blocked by:** 05

- [x] "API" 절의 응답 표가 `../spec.md`의 응답 표와 오류의 우선순위와 같다
- [x] "API" 절의 `curl` 예시를 실제 서버에 그대로 보내면 적힌 응답이 온다
- [x] L3, L10, L14의 `근거` 칸이 인용한 테스트 ID가 실제로 있다
- [x] `make verify`가 통과한다

## Comments

- "API" 절의 응답 표는 spec의 표와 같은 줄이고, 오류의 우선순위에는 spec에 없는 첫 줄을 더했습니다: `Content-Length`가 상한을 넘으면 경로와 메서드를 보기 전에 `413`입니다. 서버의 `headers_check`가 헤더 단계에서 `api_handle`보다 먼저 검사하기 때문이며, `curl -X PUT -H "Content-Length: 16777217" /nope`가 `413`을 받는 것을 실제 서버에서 확인했습니다.
- `curl` 예시 네 줄은 `./build/mini-redis 8080`에 그대로 보내 받은 응답을 옮겼습니다. "설치 및 실행"의 "포트를 생략하면 8080"과 "어떤 요청에도 `ok`" 예시는 M0의 서버 설명이라 지웠습니다. 지금 `main`은 포트가 없으면 사용법을 출력하고 2로 끝납니다.
- ID: 기능 테스트 T5~T8(L3은 T5, L14는 T5~T8), api 층 단위 테스트 T9~T11(L10, ADR `http-methods-and-raw-body`와 함께). `scripts/check-ids.py`는 `tests/` 아래 모든 파일을 읽으므로 `tests/functional/api_test.py`의 ID도 셉니다.
- README 마일스톤 표의 M2 상태는 `진행 중`으로 바꾸고, `Redis와 대조` 칸은 비워 두었습니다. M1은 6단계에서 `완료`와 그 링크를 함께 넣었고, README는 "마일스톤이 끝나면" 링크한다고 적습니다. 6단계에서 `완료`와 링크를 넣습니다.
