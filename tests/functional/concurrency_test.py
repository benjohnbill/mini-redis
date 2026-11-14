#!/usr/bin/env python3
"""기능 테스트(M3 concurrency): 실제 mini-redis 서버를 띄우고 바깥에서 연결과 요청을 보냅니다.

사용법: python3 -B tests/functional/concurrency_test.py build/mini-redis

서버를 띄우는 방식은 api_test.py와 같습니다. 서버는 한 번만 띄우고, 테스트마다 서로
다른 key를 씁니다. 테스트가 실패해도 마지막에 서버를 끕니다.
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
BODY_MAX = 16 * 1024 * 1024  # body 상한 16 MiB(16,777,216바이트), M2 spec Q10
LARGE_REQUEST_TIMEOUT = 15.0  # ASan을 켠 서버가 16 MiB를 주고받는 요청입니다
PROMPT_TIMEOUT = 0.5  # 큰 요청이 도는 중에도 작은 GET이 이 시간 안에 응답을 받아야 합니다(ASan 기준)
LARGE_CHUNK = 1024 * 1024  # 큰 value를 이 크기씩 보내거나 받습니다
CHUNKS_PER_CHECK = 4  # 큰 value를 이만큼 주고받을 때마다 작은 GET을 한 번 보냅니다


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

    def request(self, method, path, body=None, timeout=REQUEST_TIMEOUT):
        connection = http.client.HTTPConnection(HOST, self.port, timeout=timeout)
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

def large_value():
    # 바이트마다 값이 바뀌어, 잘리거나 밀린 곳이 있으면 비교가 실패합니다.
    return bytes(range(256)) * (BODY_MAX // 256)


def expect_small_get_prompt(server, key):
    """큰 요청과 별개인 새 연결의 작은 GET이 PROMPT_TIMEOUT 안에 응답을 받는지 봅니다."""
    started = time.monotonic()
    status, _, body = server.request("GET", f"/keys/{key}", timeout=PROMPT_TIMEOUT)
    elapsed = time.monotonic() - started

    expect(status, 404, "작은 GET의 상태 코드")
    expect(body, b'{"error":"key_not_found"}', "작은 GET의 body")
    if elapsed >= PROMPT_TIMEOUT:
        raise AssertionError(f"작은 GET이 {elapsed:.3f}초 걸렸습니다")


def small_get_prompt_while_storing_large_value(server):
    value = large_value()
    head = f"PUT /keys/large-store HTTP/1.1\r\nHost: x\r\nContent-Length: {len(value)}\r\n\r\n"
    with socket.create_connection((HOST, server.port), timeout=LARGE_REQUEST_TIMEOUT) as large:
        large.sendall(head.encode("ascii"))
        # 조각을 쉬지 않고 보내므로, 작은 GET이 도착할 때 서버는 아직 받지 않은 body를 읽는 중입니다.
        for index in range(len(value) // LARGE_CHUNK):
            large.sendall(value[index * LARGE_CHUNK:(index + 1) * LARGE_CHUNK])
            if index % CHUNKS_PER_CHECK == 0:
                expect_small_get_prompt(server, f"small-while-storing-{index}")
        status_line = large.makefile("rb").readline()

    expect(status_line.split(b" ")[1:2], [b"204"], "큰 저장의 상태 코드")
    status, _, body = server.request("GET", "/keys/large-store", timeout=LARGE_REQUEST_TIMEOUT)
    expect(status, 200, "큰 저장 뒤 조회의 상태 코드")
    if body != value:
        raise AssertionError("조회한 body가 저장한 큰 body와 다릅니다")


TESTS = [
    ("[T14] (기능) 다른 연결이 16 MiB value를 저장하는 중에도 작은 GET은 0.5초 안에 응답을 받는다",
     small_get_prompt_while_storing_large_value),
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
