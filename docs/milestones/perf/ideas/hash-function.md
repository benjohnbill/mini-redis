# hash 함수 로직 선택 & seed 암호화 성능 측정

## 관찰 대상

hash 함수를 바꾸면 [hash 값을 계산하는 시간]과 [key가 버킷에 흩어지는 정도가 어떻게 달라지는지] 봐요. hash 계산은 저장, 조회, 삭제의 모든 요청에서 한 번씩 일어나요. 

seed가 공격을 막는 정도는 hash 함수마다 다르므로, seed를 넣을지와 어떤 hash 함수를 쓸지를 한 실험에서 함께 정해요.

## 측정 방법

`make hash-micro`(실험 브랜치 `CJG-hash-bench`의 `scripts/bench/hash_micro.c`)로, 서버 없이 hash 함수만 불러서 재요. 세 검사는 한 번 실행할 때 모두 돌고, 처리 시간 검사만 그 안에서 3회 반복해요.

### 처리 시간

```text
시계 시작
  16바이트 key를 hash하기 16,777,216번
시계 멈춤
전체 시간 ÷ 16,777,216 = hash 한 번의 시간
```

### 평균 비교 횟수
entry에 몇 개의 key가 들어있는 지에 대한 지표입니다.

```text
예를 들어, 평균 비교 횟수가 2.0이면 GET 한 번에 entry 2개를 비교합니다.
```

### Seed에 따른 bucket 분포의 변화율

```text
seed의 bit 하나를 바꿨을 때 key 가운데 몇 %가 다른 버킷으로 옮겨 가는지를 재는 지표입니다.
```

## 결과

**결론:** SipHash 1-2를 사용합니다.

숫자는 측정 기록 [`hash-micro/001`](../../../measurements/hash-micro/001-CJG-hash-functions.md)에 있고, 결정은 ADR [`siphash-with-random-seed`](../../../adr/siphash-with-random-seed.md)에 있어요.

- **FNV는 뺐어요.** seed를 넣어도 seed의 bit 40을 바꿨을 때 모든 key의 버킷이 그대로였어요. 버킷 번호는 hash의 아래쪽 21 bit이고, FNV에서는 seed의 bit 21 이상이 그 bit에 닿지 않기 때문이에요. 바이트당 약 1.6 ns가 들어 가장 느리기도 했어요.
- **wyhash와 xxHash는 가장 빨랐지만 고르지 않았어요.** seed bit 검사는 통과했지만, 이 검사는 seed가 쓸모없는 함수를 걸러낼 뿐이에요. seed를 모르면 출력을 예측할 수 없도록 설계된 함수는 SipHash뿐이에요.
- **SipHash 2-4 대신 1-2를 골랐어요.** 1-2가 약 두 배 빨랐고, Redis도 같은 이유로 1-2를 써요. 16바이트 key에서 hash 한 번이 약 17 ns로, 지금의 FNV(약 30 ns)보다도 빨라요.
- **처음 잰 결과는 버렸어요.** 커밋하기 전의 코드로 잰 결과라서 기록으로 남길 수 없었고, 실험 브랜치의 커밋 `81fb122`에서 다시 쟀어요. 예상을 적기 전에 잰 결과예요.

## 관련 ADR 및 문서

| 문서 | 이 실험과의 관계 |
|---|---|
| `src/hash.c`의 `hash_bytes` | 이전의 hash 함수(FNV) |
| `src/store.c`의 `entry_hash` | store가 key 바이트만 hash하는 자리. 이 실험으로 SipHash 1-2를 부르게 바뀜 |
| `docs/milestones/store/spec.md` Q6 | key 바이트만 hash하고 value는 hash하지 않기로 한 결정 |
| `docs/milestones/store/spec.md` "Redis와 대조"의 해시와 비교 행 | Redis가 seed를 섞는 SipHash를 쓴다고 적은 자리 |
| `README.md` "알려진 한계" | key의 hash에 무작위 seed를 섞지 않는다는 한계가 있었고, 이 실험으로 지움 |
| [`overview.md`](../overview.md) | hash seed를 안전성 문제로 보되, hash 함수 실험과 함께 정했다고 적은 자리 |
| [RFC 9923](https://www.rfc-editor.org/rfc/rfc9923.html) §1.4, §2.2, §6.1 | FNV의 최하위 bit, 비밀 offset basis가 막는 공격과 막지 못하는 공격 |
| [Redis `src/siphash.c`](https://github.com/redis/redis/blob/unstable/src/siphash.c) | Redis가 2-4 대신 1-2를 고른 이유: 2-4가 Redis를 4% 정도 느리게 했다는 주석 |
