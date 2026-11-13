# M2 `api`: 외부에서 부르기

## 요구사항

L3, L10, L14 (`docs/requirements.md`)

## 앞 마일스톤

- M0: 서버(`src/server.c`)는 요청을 하나씩 받아, 어떤 요청에도 `ok`를 돌려줍니다. 아직 store를 부르지 않습니다.
- M1 `store`: store의 공개 함수 다섯 개(`store_create`, `store_destroy`, `store_set`, `store_get`, `store_delete`)가 key와 value를 주소와 길이 한 쌍으로 받습니다. 결정과 이유는 `docs/milestones/store/spec.md`와 ADR `key-value-as-bytes-with-length`, `get-lends-value-until-next-change`에 있습니다.
- M1이 M2로 넘긴 결정: 0바이트 key를 외부 요청에서 받을지, 그리고 `store_set`, `store_get`, `store_delete`의 `bool` 결과를 응답에서 어떻게 나타낼지입니다.

## 탐구할 문제

다른 서버가 네트워크 너머에서 mini-redis에 key와 value를 넣고, 꺼내고, 지우려 합니다.

1. 다른 서버는 어떤 규칙으로 mini-redis에 말을 걸어야 하나요? L3 해석은 HTTP로 정했습니다. 저장, 조회, 삭제라는 세 동작을 요청의 어느 부분으로 구분하나요?
2. key와 value는 요청의 어디에 실리나요? key를 URL에 넣는다면, key에 `/`, 공백, 한글, `'\0'` 바이트가 들어 있을 때는 어떻게 되나요? 0바이트 key는 받나요?
3. 응답은 무엇을 알려야 하나요? "찾았고 value는 0바이트", "key가 없음", "지운 것이 없음", "메모리 부족", "요청이 잘못됨"을 다른 서버가 서로 구분할 수 있어야 하나요?
4. 요청을 직접 해석할까요, 라이브러리를 쓸까요? L5는 서버 프레임워크를 허용합니다. 라이브러리를 쓴다면 라이선스와 의존성은 이 레포의 규칙과 맞나요?
5. `store_get`이 빌려준 value의 주소는 다음 `store_set` 또는 `store_delete` 전까지만 유효합니다. 응답을 다 보내기 전에 그 주소가 무효가 될 수 있는 경로가 서버 코드에 있나요?
6. 요청이 비정상이면 서버는 어떻게 되나요? body가 아주 크거나, 길이를 알려주지 않거나, 끝까지 오지 않는 요청이 다음 요청을 막지 않으려면 무엇이 필요한가요?
7. L10은 "쉽게 사용할 수 있도록"이라고 합니다. 다른 서버의 개발자가 코드를 읽지 않고 이 API를 쓰려면 무엇을 보면 되나요?
8. L14의 기능 테스트는 실제 서버를 띄우고 바깥에서 부르는 테스트입니다. 무엇으로 서버를 띄우고, 무엇으로 요청을 보내고, `make verify`와 CI에는 어떻게 넣나요?

## 끝나면 생기는 것

- `spec.md`
- ADR
- `tests/api_test.c`
- `tests/functional/`의 기능 테스트
