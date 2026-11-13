---
status: accepted
milestone: api
---

# 동작은 HTTP 메서드로, key는 경로로, value는 raw body로 받습니다

맥락: 다른 서버가 저장, 조회, 삭제를 요청의 어느 부분으로 구분하고, key와 value를 어디에 실을지 정해야 했습니다. value를 JSON으로 감쌀지도 정해야 했습니다.

결정: `PUT`, `GET`, `DELETE /keys/<key>`로 저장, 조회, 삭제를 구분합니다. key는 퍼센트 인코딩을 한 번만 풀어 바이트로 쓰고, 0바이트 key는 `400`으로 거절합니다. value는 body의 바이트를 그대로 쓰고, 길이는 `Content-Length`에서 얻습니다. 오류 응답의 body만 `{"error":"<오류 이유>"}` JSON으로 보냅니다.

이유: 우리가 구현하는 범위 내에서는 HTTP의 헤더와 body 부분만 있어도 충분하기 때문에, JSON을 사용할 이유가 없습니다.

버린 대안: value를 JSON 문자열로 받으면 UTF-8이 아닌 바이트를 담으려고 base64가 필요하고, 그러면 모든 클라이언트가 인코딩과 디코딩을 해야 하며 JSON의 가독성도 사라집니다. 경로로 동작을 구분하는 방식(`POST /set`)이나 body에 명령 문장을 싣는 방식은 HTTP 메서드가 이미 주는 구분을 다시 만드는 셈입니다.
