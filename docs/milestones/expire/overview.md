# M4 `expire`: 보관 기간

## 요구사항

L9 (`docs/requirements.md`)

## 앞 마일스톤

- M1 `store`: store는 key와 value를 주소와 길이 한 쌍으로 받습니다(ADR `key-value-as-bytes-with-length`). entry는 key와 value의 바이트를 함께 담고, 같은 key에 다시 넣으면 entry가 통째로 바뀝니다. `store_get`은 value를 복사하지 않고 다음 `store_set` 또는 `store_delete` 전까지 빌려줍니다(ADR `get-lends-value-until-next-change`).
- M2 `api`: 동작은 HTTP 메서드로, key는 경로로, value는 body의 바이트 그대로 받습니다(ADR `http-methods-and-raw-body`). 그래서 body에는 value 말고 다른 것을 실을 수 없습니다. api 층은 메서드, 경로, body만 받고 헤더는 받지 않습니다. 서버는 mongoose로 헤더를 읽을 수 있고, 지금은 `Transfer-Encoding`과 `Content-Length`만 봅니다.
- M3 `concurrency`: 서버는 프로세스 1개, 스레드 1개로 모든 요청을 처리합니다(ADR `single-thread-event-loop`). mongoose 루프는 100 ms 이내마다 한 번씩 돌고, 쉬는 연결을 닫는 일도 그 루프 안에서 mongoose의 시계(`mg_millis`)로 판단합니다.
- M1~M3이 M4로 넘긴 것:
  - 요청 하나를 처리하는 동안 다른 요청은 기다립니다. entry를 한 번에 많이 지우거나 key 수가 2배로 늘어나는 순간의 rehash는 서버를 멈춥니다(M3 `spec.md`의 "측정 결과"). 지연 목표는 "방해 요청이 있어도 작은 GET의 p99 10 ms 이하"이고, `make bench`로 잽니다.
  - M3의 기능 테스트는 10초 기한을 실제로 기다립니다. 테스트에 맞춰 서버 코드를 바꾸지 않기로 했기 때문입니다.
- mongoose 영향: 작습니다. 헤더를 읽는 기능은 mongoose에 이미 있고, 헤더로 싣는다면 api 층에는 인자가 하나 늘어납니다.

## 탐구할 문제

key에 보관 기간을 붙이고, 기간이 지난 값을 요청받았을 때 어떻게 처리할지 정합니다.

1. 보관 기간을 요청의 어디에 실을까요? body는 value의 바이트 그대로이므로(ADR `http-methods-and-raw-body`) 헤더나 경로 쪽이 후보입니다.
2. 단위는 초인가요, 밀리초인가요? "몇 초 뒤"인가요, "몇 시 몇 분"인가요?
3. 만료된 key를 조회하거나 지우면 응답은 무엇인가요? 없는 key와 같게 줄까요, 구분할까요?
4. 만료를 언제 판단하고, 만료된 entry의 메모리는 언제 돌려받나요? 아무도 다시 찾지 않는 key는 어떻게 되나요?
5. 보관 기간이 있는 key를 덮어쓰면 기간은 어떻게 되나요? 기간을 없앨 수도 있어야 하나요?
6. 테스트가 실제로 기다리지 않고 시간을 흘려보내려면, 시계를 어떻게 바꿔 끼울까요?
7. 시스템 시각이 바뀌어도 만료가 맞게 동작해야 하나요?

## 끝나면 생기는 것

- `spec.md`
- ADR
- `tests/expire_test.c`
- `tests/functional/`의 기능 테스트
