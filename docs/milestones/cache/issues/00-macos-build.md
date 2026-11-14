# 00: macOS에서도 기존 검증 빌드를 실행한다

**What to build:** 현재 macOS 환경에서 기존 mini-redis와 테스트를 빌드하고, Linux 설정을 유지하면서 M5의 실제 HTTP 기능 테스트를 실행할 수 있게 합니다.

**Blocked by:** None (can start immediately).

Status: ready-for-agent

- [ ] 현재 Mongoose와 Acutest 빌드 실패를 재현하고 OS별 feature macro나 시스템 선언 설정의 가장 작은 수정으로 해결한다. 가져온 코드 본문은 바꾸지 않는다.
- [ ] 기존 Linux 빌드 설정과 sanitizer 검사를 유지한다.
- [ ] 기존 테스트·ID 검사·코드 규칙 검사를 실행하고 결과와 환경 제약을 기록한다.

## Comments

- M5 구현 시작 전 확인한 macOS 빌드 실패에 따른 검증 선행 작업입니다. 01은 이 작업이 끝난 통합 브랜치에서 시작합니다.
