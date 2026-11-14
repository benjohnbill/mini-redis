# 00: macOS에서도 기존 검증 빌드를 실행한다

**What to build:** 현재 macOS 환경에서 기존 mini-redis와 테스트를 빌드하고, Linux 설정을 유지하면서 M5의 실제 HTTP 기능 테스트를 실행할 수 있게 합니다.

**Blocked by:** None (can start immediately).

Status: ready-for-agent

- [x] 현재 Mongoose와 Acutest 빌드 실패를 재현하고 OS별 feature macro나 시스템 선언 설정의 가장 작은 수정으로 해결한다. 가져온 코드 본문은 바꾸지 않는다.
- [x] 기존 Linux 빌드 설정과 sanitizer 검사를 유지한다.
- [x] 기존 테스트·ID 검사·코드 규칙 검사를 실행하고 결과와 환경 제약을 기록한다.

## Comments

- M5 구현 시작 전 확인한 macOS 빌드 실패에 따른 검증 선행 작업입니다. 01은 이 작업이 끝난 통합 브랜치에서 시작합니다.
- 2026-11-14: `make verify`의 Mongoose 선언 오류와 `make test`의 Acutest BSD 타입 오류를 재현했습니다. Darwin에서만 `_DARWIN_C_SOURCE`를 추가하고, clang-tidy에도 같은 선언과 `xcrun`으로 찾은 SDK를 전달했습니다. 가져온 코드와 Linux 플래그, sanitizer 설정은 유지했습니다.
- 검증: 고정 버전 clang-format 21.1.8·clang-tidy 21.1.6으로 `make verify`가 통과했습니다(단위 76개, 실제 HTTP 기능 32개, ID 17개, 코드 규칙 검사). `make build/bench/mini-redis` 빌드도 통과했습니다. Linux는 명령 생성으로 기존 플래그 유지만 확인했으며 실제 실행은 하지 않았습니다.
- 환경: 고정 clang 도구는 임시 가상환경에 설치했습니다. sandbox에서는 localhost bind가 거부되어 실제 HTTP 기능 테스트는 권한을 확장해 실행했습니다. macOS의 pip clang-tidy는 Xcode SDK 경로가 필요합니다.
