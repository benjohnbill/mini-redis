---
seq: TBD
topic: chain-before-pr44
date: 2026-11-18
who: CJG
commit: 66d6de7
previous: 001-CJG-baseline.md
machine: "Intel Core Ultra 7 155H(논리 코어 22개), Windows의 WSL2 Ubuntu 26.04.1(커널 6.18.58.1-microsoft-standard-WSL2, 메모리 15 GiB), gcc 15.2.0, wrk 4.1.0, Python 3.14.4"
build: "make -B build/bench/mini-redis: -O2, sanitizer 끔, assert 남김, SWEEP_TRACE 켬"
command: "python3 -B scripts/bench/idle_wakeup.py build/bench/mini-redis (#44 브랜치의 스크립트를 꺼내 실행, 상태마다 5초씩 3회)"
milestones: [perf]
data: none
---

# 병합 사슬: #44를 병합하기 전의 깨어남

## 앞 기록과 달라진 점

- **목적:** 병합 사슬에서 #44의 "개선 전"입니다. #44를 병합하기 직전의 `main`(#37, #35까지 병합)을 쟀습니다.
- **코드:** 001(`d64ae63`) 뒤에 #37(rehash 나눠옮기기)과 #35(rehash 범위)가 `src/hash.c`를 바꿨습니다. 루프의 대기 시간은 바뀌지 않았습니다.
- **도구:** `idle_wakeup.py`는 아직 `main`에 없어서, #44 브랜치의 같은 파일을 꺼내 실행했습니다. #44가 병합한 파일과 내용이 같습니다.

## 결론

- **세 상태 모두 초당 39.8번으로, 001과 같습니다.** #37과 #35는 요청이 없을 때의 깨어남을 바꾸지 않았습니다.

## 결과

| 상태 | 1회 | 2회 | 3회 |
|---|---|---|---|
| `empty`: key와 연결이 없음 | 39.8 | 39.8 | 39.8 |
| `ttl-key`: 보관 기간 1시간인 key 1개 | 39.8 | 39.8 | 39.8 |
| `idle-conn`: 아무것도 보내지 않는 연결 1개 | 39.8 | 39.8 | 39.8 |
