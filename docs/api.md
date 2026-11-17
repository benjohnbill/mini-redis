# API

mini-redis의 HTTP 계약 전체입니다. 요약은 [`README.md`](../README.md)의 "API" 절에 있습니다.

다른 서버는 HTTP로 mini-redis를 부릅니다. 동작은 메서드로, key는 경로 `/keys/<key>`로, value는 body의 바이트 그대로 나타냅니다. 여러 key를 한 번에 조회하는 `POST /mget`만 key를 body로 받습니다.

| 요청 | 동작 |
|---|---|
| `PUT /keys/<key>` | body를 value로 저장합니다. 같은 key가 있으면 덮어씁니다. `TTL` 헤더로 보관 기간을 정할 수 있습니다. |
| `GET /keys/<key>` | 저장한 value를 body로 돌려줍니다. |
| `DELETE /keys/<key>` | key를 지웁니다. |
| `POST /mget` | body에 줄마다 적은 key들의 value를 한 응답으로 돌려줍니다. 아래 "여러 key 조회"를 따릅니다. |

- **key:** `/keys/` 뒤의 나머지 전부입니다. 퍼센트 인코딩(`%HH`)을 정확히 한 번 풀고, 그 바이트 그대로 key로 씁니다. 그래서 `%2F`는 경로 구분자가 아니라 key 안의 `/`이고, `%00`이 든 key도 잘리지 않으며, `%2541`은 `%41`이라는 key가 됩니다. 한글, 공백, `/`가 든 key는 인코딩해서 보냅니다. key 안의 `?`는 반드시 `%3F`로 보냅니다. 인코딩하지 않은 `?`가 있으면 `400 bad_key_encoding`입니다. 그래서 `/keys/a?b=1` 같은 query는 쓸 수 없습니다.
- **value:** body의 바이트 그대로이고, 길이는 `Content-Length`로 알립니다. JSON으로 감싸지 않습니다. `'\0'`이나 UTF-8이 아닌 바이트가 든 value(이미지 등)도 그대로 저장하고 돌려받습니다. 0바이트 value도 저장할 수 있고, 조회하면 `200`과 0바이트 body를 받습니다.
- **보관 기간:** `PUT`에 `TTL: <초>` 헤더를 붙이면 그 key는 그 초 동안만 있습니다. 헤더가 없으면 지울 때까지 남습니다. 헤더 이름은 대소문자를 가리지 않습니다.
  - **받는 값:** 단위는 초이고, 10진수 숫자로만 된 1 이상의 정수만 받습니다. 값 앞뒤의 공백은 받습니다. 0, 음수, `+` 부호, 숫자 사이의 공백, 숫자가 아닌 값, 빈 값, 너무 큰 값(약 1.8×10¹⁶초를 넘는 값), 한 요청에 두 번 온 `TTL` 헤더는 `400 bad_ttl`이고, 저장하지 않습니다. 0을 "바로 지우기"로 받지 않는 것은 client의 실수가 조용히 데이터 삭제가 되지 않게 하기 위해서입니다.
  - **세는 시작:** 서버가 그 요청의 헤더를 다 받은 순간부터 셉니다. 큰 body를 천천히 올려도 올리는 시간만큼 늦게 만료되지 않습니다.
  - **만료:** 보관 기간이 끝나는 순간부터 그 key는 없는 key와 같습니다. `GET`과 `DELETE`는 `404 key_not_found`를 받고, "만료됨"을 따로 알리지 않습니다.
  - **덮어쓰기:** `PUT`은 key의 보관 기간도 새로 정합니다. 기간이 남은 key에 `TTL: 5`로 다시 `PUT`하면 그때부터 5초이고, `TTL` 없이 다시 `PUT`하면 기간이 사라집니다. 기간만 바꾸거나 남은 기간을 묻는 API는 없습니다.
  - **`GET`과 `DELETE`:** `GET`은 보관 기간을 늘리지 않습니다. 두 요청에 붙인 `TTL` 헤더는 무시합니다.
  - **메모리:** 아무도 다시 조회하지 않는 만료된 key도 서버가 25 ms마다 한 번에 2,000개까지 지워 메모리를 돌려받습니다.

## 여러 key 조회

`POST /mget`은 key 여러 개를 요청 하나로 조회합니다. 메서드는 `POST`만 받습니다.

- **요청 body:** key를 한 줄에 하나씩 적고, 줄은 `\n` 하나로 나눕니다. 마지막 줄의 `\n`은 없어도 됩니다. 그래서 `a\nb`와 `a\nb\n`은 같은 요청입니다. `\r`은 줄 끝으로 보지 않고 key의 바이트로 읽습니다.
- **key:** 줄마다 `/keys/<key>`의 key와 같은 규칙으로 퍼센트 인코딩을 한 번 풉니다. key 안의 줄바꿈은 `%0A`로, `?`는 `%3F`로 보냅니다. 빈 줄은 빈 key라서 `400 empty_key`이고, 깨진 인코딩이나 인코딩하지 않은 `?`는 `400 bad_key_encoding`입니다.
- **응답 body:** `200`과 `Content-Type: application/octet-stream`으로, key 순서대로 key마다 `<value 바이트 길이>\n<value 바이트>\n`을 이어 붙입니다. 없는 key와 만료된 key는 `-\n`입니다. 길이를 먼저 적으므로 `\n`이나 `'\0'`이 든 value도 그대로 돌려받습니다. 0바이트 value는 `0\n\n`입니다.
- **같은 key:** 같은 key가 여러 번 오면 나온 자리마다 매번 돌려줍니다.
- **시각:** 모든 key를 요청 하나의 같은 시각으로 판정합니다. `GET`처럼 보관 기간을 늘리지 않고, 만료된 key를 지우지도 않습니다.
- **key 수:** 1개부터 64개까지 받습니다. 빈 body는 `400 bad_request`, 65개 이상은 `400 too_many_keys`입니다.
- **value 합계:** 돌려줄 value 바이트의 합이 16 MiB(16,777,216바이트)를 넘으면 `400 response_too_large`로 거절합니다. 없는 key는 합계에 더하지 않습니다. 서버는 스레드 하나로 요청을 처리하므로, 거대한 응답 하나를 만드는 동안 다른 요청이 기다리지 않게 하려는 상한입니다.
## 상한

| 항목 | 상한 | 넘으면 |
|---|---|---|
| body | 16 MiB(16,777,216바이트), `Content-Length`로 판정 | body를 받기 전에 `413`을 보내고 연결을 닫습니다. |
| 동시 연결 | 128개 | 새로 연 연결은 `503`을 받고 닫힙니다. 열린 연결이 닫혀 자리가 나면 다시 받습니다. |
| 헤더 수 | 64개 | body를 받기 전에 `431`을 보내고 연결을 닫으며, 저장하지 않습니다. 새 연결의 요청은 정상으로 처리합니다. |
| body 길이를 알리는 헤더 | `Content-Length`만 | `Transfer-Encoding` 헤더가 있으면 값(`chunked` 등)과 상관없이 body를 받기 전에 `411`을 보내고 연결을 닫습니다. |
| 쉬는 시간 기한 | 10초 | 아래 "쉬는 시간 기한" 표를 따릅니다. |
| `POST /mget`의 key 수 | 64개 | `400 too_many_keys`를 보내고 조회하지 않습니다. |
| `POST /mget`의 value 합계 | 16 MiB(16,777,216바이트) | `400 response_too_large`를 보냅니다. |

- **연결 재사용:** 연결 하나로 요청을 여러 번 보낼 수 있습니다.
- **헤더 수의 이유:** 상한 너머의 헤더를 읽지 못한 채 처리하면 그 안의 `TTL`이 조용히 빠지기 때문입니다. 연결을 닫는 것은 숨은 `Content-Length` 때문에 다음 요청이 어디서 시작하는지 알 수 없어서입니다.
- **`Transfer-Encoding`의 이유:** chunked body는 끝까지 받아야 길이를 알 수 있어서 상한을 먼저 검사할 수 없기 때문입니다. 예외로, `Transfer-Encoding`과 `Content-Length`가 함께 오거나 `Transfer-Encoding`이 두 번 오면 형식 오류로 보고 응답 없이 연결을 닫습니다. 두 헤더가 body의 끝을 다르게 알려, 요청이 어디서 끝나는지에 답이 둘이 되기 때문입니다(RFC 9112 6.1절은 이런 요청을 거절하고 연결을 닫도록 허용합니다).

### 쉬는 시간 기한

연결에서 10초 동안 바이트가 오가지 않으면(받기와 보내기 모두) 서버가 연결을 닫습니다.

| 연결 상태 | 10초 동안 바이트가 오가지 않으면 |
|---|---|
| 요청을 보내다 멈춤 | `408`을 보내고 닫습니다. |
| 응답을 받은 뒤 쉼 | 응답 없이 닫습니다. |
| 연결만 열고 아무것도 보내지 않음 | 응답 없이 닫습니다. |
| 응답을 받다가 전혀 읽지 않음 | 응답 중간이라도 응답 없이 닫습니다. |

큰 body를 천천히 보내거나 큰 응답을 천천히 받아 가는 동안에는 바이트가 오가므로 닫히지 않습니다.

## 응답

| 상황 | 상태 코드 | body |
|---|---|---|
| 조회 성공 | `200` | value 바이트 (`Content-Type: application/octet-stream`) |
| `POST /mget` 성공 | `200` | key마다 `<길이>\n<value>\n` 또는 `-\n` (`Content-Type: application/octet-stream`) |
| 저장 성공 (새로 만듦, 덮어씀 구분 없음) | `204` | 없음 |
| 삭제 성공 | `204` | 없음 |
| 조회, 삭제할 key가 없음 (만료된 key 포함) | `404` | `{"error":"key_not_found"}` |
| `/keys/`로 시작하지 않고 `/mget`도 아닌 경로 (`/keys`, `/mget/`, `/mget?x=1`도 포함) | `404` | `{"error":"route_not_found"}` |
| 0바이트 key (`/keys/`, `POST /mget` body의 빈 줄) | `400` | `{"error":"empty_key"}` |
| 깨진 퍼센트 인코딩 (`%G1`, 끝이 잘린 `%2`), 경로나 `POST /mget` body에 인코딩하지 않은 `?` | `400` | `{"error":"bad_key_encoding"}` |
| `PUT`의 `TTL`이 1 이상의 정수가 아니거나, 너무 크거나, 두 번 옴 | `400` | `{"error":"bad_ttl"}` |
| `POST /mget`의 body가 비어 key가 없음 | `400` | `{"error":"bad_request"}` |
| `POST /mget`의 key가 65개 이상 | `400` | `{"error":"too_many_keys"}` |
| `POST /mget`이 돌려줄 value 합계가 16 MiB를 넘음 | `400` | `{"error":"response_too_large"}` |
| `/keys/<key>`에 `PUT`, `GET`, `DELETE`가 아닌 메서드 | `405` | `{"error":"method_not_allowed"}`, 헤더 `Allow: GET, PUT, DELETE` |
| `/mget`에 `POST`가 아닌 메서드 | `405` | `{"error":"method_not_allowed"}`, 헤더 `Allow: POST` |
| 요청을 보내다 10초 동안 멈춤 | `408` | `{"error":"request_timeout"}`, 보낸 뒤 연결을 닫음 |
| `Transfer-Encoding`이 있는 요청 (`chunked` 포함, `Content-Length`가 함께 오면 응답 없이 닫음) | `411` | `{"error":"length_required"}` |
| `Content-Length`가 16 MiB를 넘음 | `413` | `{"error":"body_too_large"}` |
| 헤더가 64개를 넘음 | `431` | `{"error":"too_many_headers"}`, 보낸 뒤 연결을 닫음 |
| 서버의 메모리 부족 | `500` | `{"error":"out_of_memory"}` |
| 동시 연결이 128개일 때 새 연결 | `503` | `{"error":"too_many_connections"}`, 보낸 뒤 연결을 닫음 |

오류 응답의 `Content-Type`은 `application/json`입니다. 상태 코드가 같은 오류도 body의 오류 이유로 나눌 수 있습니다.

**오류 이유:** `key_not_found`, `route_not_found`, `empty_key`, `bad_key_encoding`, `bad_ttl`, `bad_request`, `too_many_keys`, `response_too_large`, `method_not_allowed`, `request_timeout`, `length_required`, `body_too_large`, `too_many_headers`, `out_of_memory`, `too_many_connections`.

**오류의 우선순위:** 한 요청에 오류가 여럿이면 처음 걸린 하나로 답합니다.

1. 헤더가 64개를 넘으면, 다른 무엇보다 먼저 `431`입니다. 상한 너머에 `Transfer-Encoding`이나 `Content-Length`가 숨어 있을 수 있어서, `411`과 `413`을 판정할 근거가 없기 때문입니다.
2. `Transfer-Encoding`이 있으면, 경로와 메서드를 보기 전에 `411`입니다. 헤더가 도착하자마자 검사하기 때문입니다.
3. `Content-Length`가 16 MiB를 넘으면, 같은 이유로 경로와 메서드를 보기 전에 `413`입니다.
4. 경로가 `/keys/`로 시작하지 않고 `/mget`도 아니면 `404 route_not_found`입니다.
5. 메서드가 `/keys/<key>`에서 `PUT`, `GET`, `DELETE`가 아니거나 `/mget`에서 `POST`가 아니면 `405`입니다.
6. `POST /mget`은 key 수를 먼저 봅니다. key가 없으면 `400 bad_request`, 65개 이상이면 `400 too_many_keys`입니다.
7. key의 인코딩이 깨졌거나 인코딩하지 않은 `?`가 있으면 `400 bad_key_encoding`, key가 비었으면 `400 empty_key`입니다. `POST /mget`은 앞 줄부터 보고 처음 걸린 줄의 오류로 답합니다.
8. `PUT`의 `TTL`이 잘못됐으면 `400 bad_ttl`입니다.
9. `POST /mget`의 value 합계가 16 MiB를 넘으면 `400 response_too_large`입니다. 요청의 모양이 틀린 오류를, 저장된 데이터에 따라 달라지는 이 오류보다 먼저 알립니다.

예를 들어 `POST /nope`는 `404 route_not_found`이고, `POST /keys/`는 `405`입니다.

## 예시

서버를 `./build/mini-redis 8080`으로 띄운 뒤 보낸 요청과 실제 응답입니다.

저장합니다.

```console
$ curl -i -X PUT --data-binary "hello" http://127.0.0.1:8080/keys/greeting
HTTP/1.1 204 No Content

```

조회합니다.

```console
$ curl -i http://127.0.0.1:8080/keys/greeting
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Content-Length: 5

hello
```

삭제하고, 다시 조회합니다.

```console
$ curl -i -X DELETE http://127.0.0.1:8080/keys/greeting
HTTP/1.1 204 No Content

$ curl -i http://127.0.0.1:8080/keys/greeting
HTTP/1.1 404 Not Found
Content-Type: application/json
Content-Length: 25

{"error":"key_not_found"}
```

보관 기간 1초로 저장하고, 바로 조회한 뒤 1초가 지나 다시 조회합니다.

```console
$ curl -i -X PUT -H "TTL: 1" --data-binary "hello" http://127.0.0.1:8080/keys/greeting
HTTP/1.1 204 No Content

$ curl -i http://127.0.0.1:8080/keys/greeting
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Content-Length: 5

hello
$ sleep 1.2
$ curl -i http://127.0.0.1:8080/keys/greeting
HTTP/1.1 404 Not Found
Content-Type: application/json
Content-Length: 25

{"error":"key_not_found"}
```

`greeting`(`hello`)과 `empty`(0바이트)를 저장한 뒤, 없는 key `none`과 같은 key를 섞어 한 번에 조회합니다. 응답은 `5\nhello\n`, `-\n`, `0\n\n`, `5\nhello\n`을 이은 21바이트입니다.

```console
$ curl -i --data-binary $'greeting\nnone\nempty\ngreeting' http://127.0.0.1:8080/mget
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Content-Length: 21

5
hello
-
0

5
hello
```

`TTL`이 0이면 저장하지 않습니다.

```console
$ curl -i -X PUT -H "TTL: 0" --data-binary "hello" http://127.0.0.1:8080/keys/greeting
HTTP/1.1 400 Bad Request
Content-Type: application/json
Content-Length: 19

{"error":"bad_ttl"}
```

## 한계

client가 알아야 하는 동작의 한계입니다.

- **컴퓨터가 절전하는 동안에는 보관 기간이 흐르지 않습니다.** 만료는 시스템 시각이 바뀌어도 튀지 않는 단조 시계로 판정하는데, 이 시계는 절전 동안 멈춥니다. 노트북이 1시간 잠들면 key가 1시간 더 남습니다. 서버는 보통 절전하지 않으므로 받아들였습니다(ADR `monotonic-clock-for-expiry`).
- **key는 요청이 서버에 닿는 데 걸린 시간만큼 늦게 만료됩니다.** 보관 기간은 서버가 헤더를 받은 순간부터 세므로, client가 보낸 순간보다 편도 전송 지연만큼 늦게 끝납니다. 일찍 만료되는 일은 없습니다.
- 바이트를 아주 조금씩 계속 보내거나 받아 가는 연결은 쉬는 시간 기한(10초)에 걸리지 않습니다. 이런 연결이 차지할 수 있는 자리는 연결 상한 128개로 묶입니다.
- 연결 상한에 걸린 연결은 `503`을 보낸 뒤 곧바로 닫힙니다. client가 그 사이에 요청을 보내면(특히 큰 body), 서버가 읽지 않은 데이터를 남긴 채 닫으므로 커널이 연결을 리셋해서 client가 `503` 대신 연결 리셋 오류를 받을 수 있습니다. 헤더만 보고 거절하는 `411`, `413`, `431`도 같아서, body를 함께 보낸 client는 응답 대신 연결 리셋 오류를 받을 수 있습니다.
- client가 body를 보내다 스스로 연결을 끊으면, 그때까지 받은 body가 value로 저장될 수 있습니다. mongoose가 닫히는 연결의 받다 만 요청을 요청으로 넘기기 때문입니다. 서버가 먼저 닫는 경우(`408`, `411`, `413`, `431`, `503`)에는 저장하지 않습니다.
