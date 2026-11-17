# rehash가 entry를 한 번에 옮길 때와 나눠 옮길 때의 GET 지연 비교

## 관찰 대상

key 수가 꾸준히 늘어나는 입력에서, 버킷을 늘리는 rehash 한 번이 다른 요청을 얼마나 멈추게 하는지 봐요. 나눠 옮기면 그 멈춤이 사라지는 대신, 옮기는 동안 조회와 저장이 테이블 두 개를 봐야 하는 비용이 생겨요.

## 예상

아직 적지 않았어요. 측정하기 전에 실험하는 사람이 한 문장으로 적어요.

## 현재 상태와 실험군

| | 현재 상태 | 실험군 |
|---|---|---|
| 동작 | `rehash`가 entry 전부를 새 버킷 배열로 한 번에 옮겨요 | 새 버킷 배열을 만든 뒤, 요청이나 주기 작업마다 버킷 몇 개씩만 옮겨요 |

## 측정 방법

`bench.py`의 `e-grow-once` 시나리오를 그대로 써요. key 4,194,304개를 채운 상태에서 key 하나를 더 저장해 버킷을 두 배로 늘리는 rehash를 한 번 일으키고, 그동안 작은 GET의 p99와 그 저장 하나의 시간을 재요.

실험군은 `hash.c`의 `rehash`와 `hash_find`, `hash_insert`, `hash_replace`, `hash_delete`를 고쳐야 해요. `list.c`는 고치지 않아요.

## 결과

보류했어요(2026-11-18). [`rehash-thresholds.md`](rehash-thresholds.md)에서 (1, 0.5, 0.25)를 고르면서 가장 긴 멈춤이 길어지는 것을 받아들였고([`server/007`](../../../measurements/server/007-CJG-rehash-thresholds-after.md)), 이 실험은 그 뒤로 미뤘어요.

## 관련 ADR 및 문서

| 문서 | 이 실험과의 관계 |
|---|---|
| [`server/003`](../../../measurements/server/003-DA-slower-rehash.md) E | 현재 상태의 멈춤: 버킷을 한 번 늘릴 때 작은 GET p99 212에서 253 ms |
| [`server/005`](../../../measurements/server/005-CJG-baseline.md) E | 같은 멈춤을 다시 잰 기준선: 작은 GET p99 443에서 587 ms |
| `docs/requirements.md` L8 | 이 멈춤 때문에 L8이 "부분"이고, 나눠 옮기기는 하지 않는다고 적혀 있음 |
| `docs/milestones/perf/overview.md` | 이 멈춤을 "범용" 원인으로 분류한 표 |
| `docs/milestones/store/spec.md` "Redis와 대조"의 rehash 행 | 나눠 옮기는 방식의 예 |
