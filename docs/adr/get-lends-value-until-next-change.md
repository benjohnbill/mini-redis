---
status: accepted
milestone: store
---

# 조회는 value를 복사하지 않고 다음 변경 전까지 빌려줍니다

맥락: `store_get`이 value를 돌려줄 때 store 안의 주소를 빌려줄지, 복사본을 만들어 줄지 정해야 했습니다. 빌려준 주소는 같은 key를 덮어쓰거나 삭제하면 해제된 메모리를 가리킵니다.

결정: `store_get`은 "찾았는가"를 `bool`로 돌려주고, value는 store 안의 읽기 전용 주소와 길이로 빌려줍니다. 그 주소는 같은 store에 다음 `store_set` 또는 `store_delete`를 부르기 전까지만 유효합니다.

이유: 요청을 하나씩 끝까지 처리하면, 요청을 받고 반환하는 과정 중간에 같은 key를 덮어쓰거나 삭제하는 일이 일어나지 않습니다.

결과: 스레드 수는 M3 `concurrency`에서 정하며, 여러 스레드를 고르면 이 결정을 새 ADR로 다시 정합니다. 같은 코드 안에서 조회한 뒤 store를 바꾸고 옛 주소를 쓰는 실수는 테스트에서 AddressSanitizer가 잡습니다.
