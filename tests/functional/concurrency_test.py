#!/usr/bin/env python3
"""기능 테스트(M3 concurrency): 실제 mini-redis 서버를 띄우고 바깥에서 연결과
요청을 보냅니다.

사용법: python3 -B tests/functional/concurrency_test.py build/mini-redis

서버를 띄우는 방법은 api_test.py와 같습니다. 테스트마다 서로 다른 key를 쓰고,
테스트가 연 연결은 테스트가 끝날 때 모두 닫습니다. 쉬는 연결은 10초 뒤에
닫히므로(spec Q4), 연결을 여는 테스트는 그보다 훨씬 빨리 끝나야 합니다. 마지막에
서버를 끄고, 서버의 종료 코드와 sanitizer 출력을 확인합니다.
python3 표준 라이브러리만 씁니다.
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
CONNECTION_MAX = 128  # 동시 연결의 상한, spec Q8
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


def keep_alive_get(connection, key):
    """열어 둔 연결로 없는 key를 조회하고 (상태 코드, body)를 돌려줍니다. 연결은
    닫지 않습니다."""
    connection.request("GET", f"/keys/{key}")
    response = connection.getresponse()
    return response.status, response.read()


def expect_not_found(connection, key, what):
    status, body = keep_alive_get(connection, key)
    expect(status, 404, f"{what}의 상태 코드")
    expect(body, b'{"error":"key_not_found"}', f"{what}의 body")


def open_full(server, prefix):
    """연결을 CONNECTION_MAX개 열고 돌려줍니다. 연결마다 요청 하나에 응답을 받아,
    서버가 그 연결을 받아 세었음을 확인합니다."""
    connections = []
    try:
        for index in range(CONNECTION_MAX):
            connection = http.client.HTTPConnection(HOST, server.port, timeout=REQUEST_TIMEOUT)
            connections.append(connection)
            expect_not_found(connection, f"{prefix}-{index}", f"{index + 1}번째 연결의 조회")
    except Exception:
        close_all(connections)
        raise
    return connections


def close_all(connections):
    for connection in connections:
        connection.close()


def read_rejection(rejected):
    """요청을 보내지 않은 연결에서 응답 하나를 읽고, 그 뒤 서버가 연결을 닫는지
    봅니다. 요청을 보내지 않는 이유: 서버가 읽지 않은 데이터를 남긴 채 연결을
    닫으면 커널이 연결을 리셋해서, 클라이언트가 응답을 받지 못할 수 있습니다."""
    response = http.client.HTTPResponse(rejected)
    response.begin()
    status, content_type, body = response.status, response.getheader("Content-Type"), response.read()
    rest = rejected.recv(1)
    return status, content_type, body, rest


def expect_rejected(status, content_type, body, rest):
    expect(status, 503, "넘친 연결의 상태 코드")
    expect(content_type, "application/json", "넘친 연결의 Content-Type")
    expect(body, b'{"error":"too_many_connections"}', "넘친 연결의 body")
    expect(rest, b"", "응답 뒤에 남은 바이트(닫히면 비어 있음)")


# ── 테스트 ────────────────────────────────────────────────────

def connection_over_limit_gets_503_and_closed(server):
    connections = open_full(server, "limit-full")
    try:
        with socket.create_connection((HOST, server.port), timeout=REQUEST_TIMEOUT) as rejected:
            expect_rejected(*read_rejection(rejected))
    finally:
        close_all(connections)


# 서버를 끄는 확인은 main의 마지막에 합니다.
TESTS = [
    ("[T13] (기능) 연결 128개가 열려 있을 때 129번째 연결은 503과 too_many_connections를 받고 닫힌다",
     connection_over_limit_gets_503_and_closed),
]


def server_stop_check(server):
    """서버를 끄고, sanitizer 오류 없이 0으로 끝났는지 봅니다."""
    code = server.stop()
    output = server.output()
    for mark in SANITIZER_MARKS:
        if mark in output:
            raise AssertionError(f"서버 출력에 sanitizer 오류가 있습니다:\n{output}")
    expect(code, 0, "서버의 종료 코드")


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
        server_stop_check(server)
    except AssertionError as error:
        failed = max(failed, 1)
        print(f"서버를 띄우거나 끄는 데 실패했습니다: {error}")
    finally:
        server.stop()
    if failed:
        print(f"FAILED: {failed} of {len(TESTS)} functional tests have failed.")
        return 1
    print("SUCCESS: No functional tests have failed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
