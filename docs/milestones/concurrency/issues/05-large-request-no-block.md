# 05: 큰 요청이 도는 중에도 작은 GET이 바로 응답받는다

**Status:** ready-for-agent

**What to build:** 다른 client가 16 MiB value를 저장하거나 조회하는 중에도, 작은 GET은 넉넉한 기한(0.5초) 안에 응답을 받습니다. 이것은 "느린 요청이 다른 요청을 막지 않는다"를 `make verify`에서 확인하는 티켓이고, 서버 코드는 바꾸지 않습니다. 결과가 예상과 다르면 구현하지 말고 팀에 가져옵니다(M2 티켓 05와 같은 방식). 지연의 숫자는 02의 측정이 맡습니다. 결정은 `../spec.md`의 Q11과 Testing Decisions를 따릅니다.

**Blocked by:** None (can start immediately)

테스트 시나리오(팀 확인 전). 테스트 이름은 아래 문장을 그대로 씁니다.

- [ ] [T14] (기능) 다른 연결이 16 MiB value를 저장하는 중에도 작은 GET은 0.5초 안에 응답을 받는다
- [ ] (기능) 다른 연결이 16 MiB value를 조회해 받아 가는 중에도 작은 GET은 0.5초 안에 응답을 받는다

그 밖의 완료 조건:

- [ ] 0.5초는 ASan 빌드 기준의 넉넉한 기한이다. 서버를 SIGSTOP으로 멈추면 같은 확인이 실패하는 것도 한 번 따로 확인해 `## Comments`에 적는다
- [ ] `make verify`가 통과한다
