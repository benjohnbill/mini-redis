#!/usr/bin/env python3
"""기능 테스트(M4 expire): 실제 mini-redis 서버를 띄우고 TTL 헤더로 저장한 key가
보관 기간 뒤에 만료되는지 바깥에서 확인합니다.

사용법: python3 -B tests/functional/expire_test.py build/mini-redis

서버를 띄우는 방식은 concurrency_test.py와 같습니다. 서버는 한 번만 띄우고, 테스트마다
서로 다른 key를 씁니다. 모든 테스트가 서버의 실제 시계로 WAIT_SECONDS를 기다리므로
(spec "시계"), 스레드로 함께 돌려 기다림이 테스트 수만큼 쌓이지 않게 합니다. 테스트가
실패해도 마지막에 서버를 끄고, 종료 코드와 sanitizer 출력을 확인합니다.
python3 표준 라이브러리만 쓰므로 CI에 새 의존성이 없습니다.
"""

import http.client
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

HOST = "127.0.0.1"
START_TIMEOUT = 10.0
STOP_TIMEOUT = 10.0
REQUEST_TIMEOUT = 5.0
# TTL 1초로 저장한 key를 조회하기 전에 기다리는 시간입니다. 서버는 헤더를 받은
# 순간부터 세므로, 저장 요청이 돌아온 뒤 1초를 넘게 기다리면 만료돼 있어야 합니다.
WAIT_SECONDS = 2.0
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

    def request(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection(HOST, self.port, timeout=REQUEST_TIMEOUT)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            return response.status, response.read()
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


def put(server, key, value, ttl=None):
    headers = {} if ttl is None else {"TTL": ttl}
    status, _ = server.request("PUT", f"/keys/{key}", body=value, headers=headers)
    expect(status, 204, f"{key} 저장의 상태 코드")


# ── 테스트: 보관 기간 ─────────────────────────────────────────

def key_with_ttl_expires(server):
    put(server, "ttl-one", b"short", ttl="1")
    time.sleep(WAIT_SECONDS)

    status, body = server.request("GET", "/keys/ttl-one")
    expect(status, 404, "만료된 key 조회의 상태 코드")
    expect(body, b'{"error":"key_not_found"}', "만료된 key 조회의 body")


def key_without_ttl_stays(server):
    put(server, "no-ttl", b"forever")
    time.sleep(WAIT_SECONDS)

    status, body = server.request("GET", "/keys/no-ttl")
    expect(status, 200, "TTL 없이 저장한 key 조회의 상태 코드")
    expect(body, b"forever", "TTL 없이 저장한 key 조회의 body")


def overwrite_without_ttl_clears_expiry(server):
    put(server, "ttl-cleared", b"short", ttl="1")
    put(server, "ttl-cleared", b"kept")
    time.sleep(WAIT_SECONDS)

    status, body = server.request("GET", "/keys/ttl-cleared")
    expect(status, 200, "TTL 없이 덮어쓴 key 조회의 상태 코드")
    expect(body, b"kept", "TTL 없이 덮어쓴 key 조회의 body")


# 모두 WAIT_SECONDS를 기다리므로 스레드로 함께 돌립니다.
WAITING_TESTS = [
    ("[T16] (기능) TTL 1로 저장한 key는 2초 뒤 404와 key_not_found를 받는다", key_with_ttl_expires),
    ("(기능) TTL 없이 저장한 key는 2초 뒤에도 조회된다", key_without_ttl_stays),
    ("(기능) TTL로 저장한 key를 TTL 없이 덮어쓰면 2초 뒤에도 조회된다", overwrite_without_ttl_clears_expiry),
]


def test_run(server, test):
    """테스트를 돌리고, 실패면 그 오류를, 성공이면 None을 돌려줍니다."""
    try:
        test(server)
        return None
    except Exception as error:  # 한 테스트가 실패해도 나머지를 돌립니다.
        return error


def tests_run_together(server, tests):
    """테스트마다 스레드 하나를 띄우고, 모두 끝나면 목록 순서대로 결과를 돌려줍니다."""
    errors = [None] * len(tests)

    def run(index, test):
        errors[index] = test_run(server, test)

    threads = [threading.Thread(target=run, args=(index, test))
               for index, (_, test) in enumerate(tests)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    return errors


def results_print(tests, errors):
    """결과를 출력하고 실패한 테스트 수를 돌려줍니다."""
    for (name, _), error in zip(tests, errors):
        if error is None:
            print(f"Test {name}... [ OK ]")
        else:
            print(f"Test {name}... [ FAILED ]\n  {error}")
    return sum(error is not None for error in errors)


def stop_and_find_problem(server):
    """서버를 끄고, 0으로 끝나지 않았거나 sanitizer 오류가 있으면 그 설명을 돌려줍니다."""
    code = server.stop()
    output = server.output()
    if any(mark in output for mark in SANITIZER_MARKS):
        return f"서버 출력에 sanitizer 오류가 있습니다:\n{output}"
    if code != 0:
        return f"서버의 종료 코드가 0이 아닌 {code}입니다:\n{output}"
    return None


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    server = Server(sys.argv[1])
    total = len(WAITING_TESTS)
    failed = 0
    try:
        server.wait_until_listening()
        failed += results_print(WAITING_TESTS, tests_run_together(server, WAITING_TESTS))
    except AssertionError as error:
        failed = total
        print(f"서버를 띄우지 못했습니다: {error}")
    finally:
        problem = stop_and_find_problem(server)
    if problem is not None:
        print(problem)
    if failed or problem is not None:
        print(f"FAILED: {failed} of {total} functional tests have failed.")
        return 1
    print("SUCCESS: No functional tests have failed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
