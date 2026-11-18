---
status: accepted
milestone: perf
---

# store는 key를 store마다 무작위 seed를 섞는 SipHash 1-2로 hash합니다

| 항목 | 내용 |
|---|---|
| 맥락 | store는 key를 seed 없는 FNV(`hash_bytes`)로 hash했습니다. 그래서 서버 밖에서 같은 버킷에 떨어지는 key를 미리 계산해 보내면 그 버킷의 리스트가 길어져 조회가 느려졌습니다(README "알려진 한계"). FNV에 seed를 넣어도 버킷 번호를 정하는 것은 seed의 아래쪽 bit뿐이었습니다([`hash-micro/001`](../measurements/hash-micro/001-CJG-hash-functions.md)). |
| 결정 | store를 만들 때 `getentropy`로 16바이트 seed를 정하고, key를 그 seed를 섞는 SipHash 1-2로 hash합니다. SipHash는 [참조 구현](https://github.com/veorq/SipHash)을 `vendor/siphash/`에 그대로 두고, 라운드 수만 빌드할 때 1과 2로 정합니다. value는 지금처럼 hash하지 않습니다. |
| 이유 | CJG: "그럼 FNV는 폐기하는게 맞겠네." "SipHash로 할게." |
| 버린 대안 | FNV에 seed 넣기(seed의 bit 21 이상이 버킷 번호에 닿지 않음), wyhash와 xxHash(가장 빠르지만 seed로 공격을 막는 것을 설계 목표로 두지 않음), SipHash 2-4(표준 라운드 수지만 1-2보다 약 두 배 느림). |
| 결과 | key의 버킷 위치를 서버 밖에서 계산할 수 없습니다. hash 한 번은 16바이트 key에서 FNV 약 30 ns보다 빠른 약 17 ns이고, 4,096바이트 key에서는 FNV 약 6.5 µs에서 약 1.3 µs로 줄어듭니다. 외부 코드(CC0) 두 파일이 늘고, `getentropy`가 실패하면 `store_create`가 `NULL`을 돌려줍니다. Redis도 같은 SipHash 1-2와 16바이트 seed를 씁니다. |
