---
status: accepted
milestone: store
---

# key와 value를 주소와 길이 한 쌍으로 받습니다

맥락: store의 공개 함수가 key와 value를 `'\0'`으로 끝나는 C 문자열로 받을지, 주소와 길이 한 쌍으로 받을지 정해야 했습니다.

결정: 모든 공개 함수는 key와 value를 `const void *`와 `size_t` 길이 한 쌍으로 받고, store는 HTTP 라이브러리의 타입을 쓰지 않습니다.

이유: HTTP 요청에서 꺼낸 key와 value는 받은 버퍼의 한 구간이라서 끝에 `'\0'`이 없고, value는 중간에 `'\0'`을 포함할 수 있습니다. value의 길이는 `Content-Length`가 이미 알려주므로 따로 구할 필요가 없습니다.

버린 대안: C 문자열은 `hash_string`을 그대로 쓸 수 있어 코드가 짧지만, 중간에 `'\0'`이 있는 value가 잘리고 M2에서 공개 함수의 모양을 다시 바꿔야 합니다.
