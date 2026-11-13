#!/usr/bin/env python3
"""기능 테스트: 실제 mini-redis 서버를 띄우고 바깥에서 HTTP 요청을 보냅니다.

사용법: python3 -B tests/functional/api_test.py build/mini-redis

서버는 한 번만 띄우고, 테스트마다 서로 다른 key를 씁니다. 빈 포트를 골라 서버를
띄우고, 연결될 때까지 기다린 뒤 테스트를 돌립니다. 테스트가 실패해도 마지막에
서버를 끄고, 서버의 종료 코드와 sanitizer 출력을 확인합니다.
python3 표준 라이브러리만 쓰므로 CI에 새 의존성이 없습니다.
"""

import http.client
import signal
import socket
import subprocess
import sys
import tempfile
import time

HOST = "127.0.0.1"
START_TIMEOUT = 10.0
STOP_TIMEOUT = 10.0
REQUEST_TIMEOUT = 5.0
SANITIZER_MARKS = ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:")


class Server:
    def __init__(self, binary):
        self.port = free_port()
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen([binary, str(self.port)],
                                        stdout=self.log, stderr=self.log)

    def wait_until_listening(self):
        deadline = time.monotonic() + START_TIMEOUT
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise AssertionError(f"서버가 시작 중에 끝났습니다: 종료 코드 {self.process.returncode}\n{self.output()}")
            try:
                socket.create_connection((HOST, self.port), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.02)
        raise AssertionError(f"서버가 {START_TIMEOUT}초 안에 연결을 받지 않습니다")

    def request(self, method, path, body=None):
        connection = http.client.HTTPConnection(HOST, self.port, timeout=REQUEST_TIMEOUT)
        try:
            connection.request(method, path, body=body)
            response = connection.getresponse()
            return response.status, response.getheader("Content-Type"), response.read()
        finally:
            connection.close()

    def stop(self):
        """SIGTERM을 보내고 종료 코드를 돌려줍니다. 제때 끝나지 않으면 죽입니다."""
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=STOP_TIMEOUT)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        return self.process.returncode

    def output(self):
        self.log.seek(0)
        return self.log.read().decode("utf-8", "replace")


def free_port():
    with socket.socket() as probe:
        probe.bind((HOST, 0))
        return probe.getsockname()[1]


def expect(actual, want, what):
    if actual != want:
        raise AssertionError(f"{what}: 기대 {want!r}, 실제 {actual!r}")


# ── 테스트 ────────────────────────────────────────────────────

def put_then_get_returns_value(server):
    status, _, _ = server.request("PUT", "/keys/roundtrip", body=b"jungle")
    expect(status, 204, "저장의 상태 코드")

    status, content_type, body = server.request("GET", "/keys/roundtrip")

    expect(status, 200, "조회의 상태 코드")
    expect(content_type, "application/octet-stream", "조회의 Content-Type")
    expect(body, b"jungle", "조회의 body")


def binary_value_round_trips(server):
    # '\0'이 가운데와 끝에 있고, UTF-8로 풀 수 없는 바이트(0xFF, 0xC3 단독)가 든 value입니다.
    value = b"a\x00b\xff\xfe\xc3\x00"
    status, _, _ = server.request("PUT", "/keys/binary", body=value)
    expect(status, 204, "저장의 상태 코드")

    status, content_type, body = server.request("GET", "/keys/binary")

    expect(status, 200, "조회의 상태 코드")
    expect(content_type, "application/octet-stream", "조회의 Content-Type")
    expect(body, value, "조회의 body")


def encoded_nul_key_is_not_truncated(server):
    status, _, _ = server.request("PUT", "/keys/nul%00key", body=b"v")
    expect(status, 204, "저장의 상태 코드")

    status, _, body = server.request("GET", "/keys/nul%00key")
    # '\0'에서 잘렸다면 key "nul"로 저장됐을 것입니다.
    truncated_status, _, truncated_body = server.request("GET", "/keys/nul")

    expect(status, 200, "nul%00key 조회의 상태 코드")
    expect(body, b"v", "nul%00key 조회의 body")
    expect(truncated_status, 404, "nul 조회의 상태 코드")
    expect(truncated_body, b'{"error":"key_not_found"}', "nul 조회의 body")


def stop_exits_zero_without_sanitizer_errors(server):
    code = server.stop()

    output = server.output()
    for mark in SANITIZER_MARKS:
        if mark in output:
            raise AssertionError(f"서버 출력에 sanitizer 오류가 있습니다:\n{output}")
    expect(code, 0, "서버의 종료 코드")


# 서버를 끄는 테스트는 마지막에 둡니다.
TESTS = [
    ("(기능) 실제 서버에 저장한 value를 조회로 돌려받는다", put_then_get_returns_value),
    ("(기능) '\\0'과 UTF-8이 아닌 바이트가 든 value를 그대로 돌려받는다", binary_value_round_trips),
    ("(기능) %00이 든 key가 실제 서버를 지나도 잘리지 않는다", encoded_nul_key_is_not_truncated),
    ("(기능) 서버를 끄면 sanitizer 오류 없이 0으로 끝난다", stop_exits_zero_without_sanitizer_errors),
]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    server = Server(sys.argv[1])
    failed = 0
    try:
        server.wait_until_listening()
        for name, test in TESTS:
            try:
                test(server)
                print(f"Test {name}... [ OK ]")
            except Exception as error:  # 한 테스트가 실패해도 나머지를 돌립니다.
                failed += 1
                print(f"Test {name}... [ FAILED ]\n  {error}")
    except AssertionError as error:
        failed = len(TESTS)
        print(f"서버를 띄우지 못했습니다: {error}")
    finally:
        server.stop()
    if failed:
        print(f"FAILED: {failed} of {len(TESTS)} functional tests have failed.")
        return 1
    print("SUCCESS: No functional tests have failed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
