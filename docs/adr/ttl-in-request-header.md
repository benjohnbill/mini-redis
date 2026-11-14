---
status: proposed
milestone: expire
---

# 보관 기간은 PUT 요청의 TTL 헤더로 받습니다

맥락: client가 key마다 보관 기간을 정하려면, 요청의 어딘가에 그 값을 실어야 했습니다. body는 value의 바이트 그대로이고(ADR `http-methods-and-raw-body`), 경로의 인코딩하지 않은 `?`는 `400 bad_key_encoding`으로 거절하며, key는 `/keys/` 뒤의 나머지 전부입니다.

결정: client는 `PUT` 요청에 `TTL: <초>` 헤더를 붙여 보관 기간을 보냅니다. 값은 1 이상의 정수만 받고, 그 밖의 값이나 두 번 온 헤더는 `400 bad_ttl`로 거절합니다. `GET`과 `DELETE`에 붙은 `TTL`은 무시합니다.

이유: (2단계에서 팀원이 말한 문장을 받아 적습니다. 문장을 받으면 status를 accepted로 바꿉니다.)

버린 대안: query string(`?ttl=60`)은 URL만 보고 기간을 알 수 있지만, `?` 거절 규칙을 바꿔야 하고, 인코딩하지 않은 `?`가 든 key가 조용히 다른 key에 저장되는 문제가 다시 생깁니다. body JSON은 위 ADR을 뒤집고, 별도 경로는 key 규칙과 부딪칩니다.
