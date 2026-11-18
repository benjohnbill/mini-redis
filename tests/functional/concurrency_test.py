#!/usr/bin/env python3
"""기능 테스트(M3 concurrency): 실제 mini-redis 서버를 띄우고 바깥에서 연결과 요청을 보냅니다.

사용법: python3 -B tests/functional/concurrency_test.py build/mini-redis

서버를 띄우는 방식은 api_test.py와 같습니다. 서버는 한 번만 띄우고, 테스트마다 서로
다른 key를 씁니다. 시간을 재는 테스트(TESTS)를 하나씩 먼저 돌리고, 서버의 쉬는 시간
기한 10초를 실제로 기다리는 테스트(WAITING_TESTS, spec "10초 기다림")는 스레드로
함께 돌려 기다림이 테스트 수만큼 쌓이지 않게 합니다. 함께 도는 연결이 앞의 시간
측정을 흔들지 않도록 두 묶음을 나눕니다. 연결 수 상한 테스트는 TESTS에 있고, 연
연결을 테스트가 끝날 때 모두 닫으며, 기한 10초보다 훨씬 빨리 끝납니다. 테스트가
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
RECV_SIZE = 4096  # recv 한 번에 받는 최대 바이트 수입니다
HEAD_END = b"\r\n\r\n"  # 헤더의 끝입니다. 마지막 헤더 줄의 줄바꿈과 빈 줄입니다
BODY_MAX = 16 * 1024 * 1024  # body 상한 16 MiB(16,777,216바이트), M2 spec Q10
LARGE_REQUEST_TIMEOUT = 15.0  # ASan을 켠 서버가 16 MiB를 주고받는 요청입니다
PROMPT_TIMEOUT = 0.5  # 큰 요청이 도는 중에도 작은 GET이 이 시간 안에 응답을 받아야 합니다(ASan 기준)
LARGE_CHUNK = 1024 * 1024  # 큰 value를 이 크기씩 보내거나 받습니다
CHUNKS_PER_CHECK = 4  # 큰 value를 이만큼 주고받을 때마다 작은 GET을 한 번 보냅니다
IDLE_TIMEOUT = 10.0  # 서버의 쉬는 시간 기한입니다(spec Q4)
# 기한이 지난 뒤 서버가 닫을 때까지 기다리는 여유입니다. 서버는 100 ms마다
# 기한을 보고, ASan 빌드는 느리므로 넉넉히 둡니다.
CLOSE_GRACE = 5.0
# 기한보다 일찍 닫았는지 볼 때, 테스트가 시각을 잰 때와 서버가 바이트를 읽은
# 때의 차이를 덮는 여유입니다.
EARLY_MARGIN = 0.5
STEADY_BODY_CHUNKS = 12  # 1초마다 한 조각씩, 모두 12초 동안 보냅니다
STEADY_BODY_INTERVAL = 1.0
STEADY_BODY_CHUNK = 1024
# 16 MiB 응답을 이 시간에 걸쳐 고르게 읽습니다. 커널의 소켓 버퍼(서버의 송신
# 버퍼와 client의 수신 버퍼, 재 보니 합쳐 약 3 MB)가 응답 끝부분을 먼저 받아
# 두어도, 서버의 마지막 보내기가 요청을 받은 때로부터 10초 뒤에 오도록 길게
# 잡습니다. 받기만 세는 서버는 이 테스트에서 약 11.6초에 연결을 닫았습니다.
SLOW_READ_SECONDS = 14.0
SLOW_READ_CHUNK = 16 * 1024
# client의 수신 소켓 버퍼입니다. 작게 고정해야 커널이 응답을 미리 다 받아 두지 못하고,
# 서버가 client의 읽는 속도에 맞춰 조금씩 보내게 됩니다.
SLOW_READ_BUFFER = 64 * 1024
SANITIZER_MARKS = ("ERROR: AddressSanitizer", "ERROR: LeakSanitizer", "runtime error:")
CONNECTION_MAX = 128  # 동시 연결의 상한, spec Q8


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

    def connect(self):
        return socket.create_connection((HOST, self.port), timeout=REQUEST_TIMEOUT)

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


# ── 테스트: 큰 요청 중의 작은 GET ─────────────────────────────

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


def read_response_head(stream):
    """상태 줄과 헤더를 읽어 (상태 코드, Content-Length)를 돌려줍니다."""
    status = int(stream.readline().split(b" ")[1])
    content_length = None
    for line in iter(stream.readline, b"\r\n"):
        name, _, field = line.partition(b":")
        if name.strip().lower() == b"content-length":
            content_length = int(field)
    return status, content_length


def small_get_prompt_while_downloading_large_value(server):
    value = large_value()
    status, _, _ = server.request("PUT", "/keys/large-download", body=value, timeout=LARGE_REQUEST_TIMEOUT)
    expect(status, 204, "큰 저장의 상태 코드")

    with socket.create_connection((HOST, server.port), timeout=LARGE_REQUEST_TIMEOUT) as large:
        large.sendall(b"GET /keys/large-download HTTP/1.1\r\nHost: x\r\n\r\n")
        stream = large.makefile("rb")
        status, content_length = read_response_head(stream)
        # 조금씩 받으므로, 작은 GET이 도착할 때 서버는 아직 보내지 못한 body를 들고 있습니다.
        received = []
        for index in range(len(value) // LARGE_CHUNK):
            if index % CHUNKS_PER_CHECK == 0:
                expect_small_get_prompt(server, f"small-while-downloading-{index}")
            received.append(stream.read(LARGE_CHUNK))
        body = b"".join(received)

    expect(status, 200, "큰 조회의 상태 코드")
    expect(content_length, len(value), "큰 조회의 Content-Length")
    if body != value:
        raise AssertionError("받아 간 body가 저장한 큰 body와 다릅니다")


# ── 테스트: 연결 수 상한 ──────────────────────────────────────

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


def connection_over_limit_gets_503_and_closed(server):
    connections = open_full(server, "limit-full")
    try:
        with server.connect() as rejected:
            expect_rejected(*read_rejection(rejected))
    finally:
        close_all(connections)


def freed_slot_accepts_new_connection(server):
    connections = open_full(server, "freed-full")
    try:
        connections.pop().close()
        replacement = http.client.HTTPConnection(HOST, server.port, timeout=REQUEST_TIMEOUT)
        connections.append(replacement)

        expect_not_found(replacement, "freed-new", "자리가 난 뒤 새 연결의 조회")
    finally:
        close_all(connections)


def open_connections_served_while_rejecting(server):
    connections = open_full(server, "serving-full")
    try:
        with server.connect() as rejected:
            # 서버가 넘친 연결을 받아 거절하는 동안 열린 연결 하나로 요청합니다.
            expect_not_found(connections[0], "serving-first", "열린 연결의 조회")
            rejection = read_rejection(rejected)
        expect_not_found(connections[-1], "serving-last", "거절 뒤 열린 연결의 조회")

        expect_rejected(*rejection)
    finally:
        close_all(connections)


# ── 테스트: 헤더만 보고 거절한 요청 ───────────────────────────

def wait_until_closed(connection):
    """서버가 연결을 닫을 때까지 기다립니다. 받은 바이트는 확인하지 않습니다.
    서버가 읽지 않은 데이터를 남긴 채 닫으면 커널이 연결을 리셋해 응답이 오지 않을
    수 있으므로, 리셋도 닫힌 것으로 봅니다."""
    try:
        while connection.recv(RECV_SIZE):
            pass
    except ConnectionResetError:
        pass
    except socket.timeout:
        raise AssertionError(f"서버가 {REQUEST_TIMEOUT:.0f}초 동안 거절한 연결을 닫지 않습니다") from None


def body_sent_with_rejected_headers_is_not_stored(server):
    head = f"PUT /keys/rejected-body HTTP/1.1\r\nHost: x\r\nContent-Length: {BODY_MAX + 1}\r\n\r\n"
    with server.connect() as rejected:
        # 헤더와 body 앞 10바이트를 한 번에 보내, 서버가 413을 정할 때 body 조각을 이미 받아 두게 합니다.
        rejected.sendall(head.encode("ascii") + b"0123456789")
        # 서버가 연결을 닫을 때 받아 둔 body 조각을 저장하지 않는지 보려고, 닫힌 뒤에 조회합니다.
        wait_until_closed(rejected)

    status, _, body = server.request("GET", "/keys/rejected-body")
    expect(status, 404, "거절한 저장 뒤 조회의 상태 코드")
    expect(body, b'{"error":"key_not_found"}', "거절한 저장 뒤 조회의 body")


# ── 테스트: 쉬는 시간 기한 ────────────────────────────────────

def read_until_closed(connection):
    """서버가 연결을 닫을 때까지 받은 바이트와 걸린 초를 돌려줍니다. 기한에
    여유를 더한 시간 안에 닫지 않으면 실패합니다."""
    connection.settimeout(IDLE_TIMEOUT + CLOSE_GRACE)
    started = time.monotonic()
    received = b""
    while True:
        try:
            chunk = connection.recv(RECV_SIZE)
        except socket.timeout:
            raise AssertionError(f"서버가 {IDLE_TIMEOUT + CLOSE_GRACE:.0f}초 동안 연결을 닫지 않습니다") from None
        if not chunk:
            return received, time.monotonic() - started
        received += chunk


def expect_not_early(elapsed, what):
    if elapsed < IDLE_TIMEOUT - EARLY_MARGIN:
        raise AssertionError(f"{what}: 기한 {IDLE_TIMEOUT:.0f}초보다 이른 {elapsed:.2f}초에 닫혔습니다")


def expect_request_timeout(received, elapsed):
    """멈춘 뒤 기한이 지나 408 request_timeout 응답 하나를 받고 닫혔는지 봅니다."""
    expect_not_early(elapsed, "408 응답")
    head, _, body = received.partition(HEAD_END)
    lines = head.split(b"\r\n")
    headers = dict(line.split(b": ", 1) for line in lines[1:] if b": " in line)
    expect(lines[0].split(b" ")[1:2], [b"408"], "상태 코드")
    expect(headers.get(b"Content-Type"), b"application/json", "Content-Type")
    expect(body, b'{"error":"request_timeout"}', "body")


def stalled_headers_get_408_and_close(server):
    # 요청 줄과 헤더 일부만 보내고 빈 줄(헤더의 끝)은 보내지 않습니다.
    with server.connect() as stalled:
        stalled.sendall(b"PUT /keys/stalled-headers HTTP/1.1\r\nHost: x\r\nContent-Le")
        received, elapsed = read_until_closed(stalled)

    expect_request_timeout(received, elapsed)


def stalled_body_gets_408_and_close(server):
    # 헤더는 다 보내고, 알린 body 100바이트 중 10바이트만 보냅니다.
    with server.connect() as stalled:
        stalled.sendall(b"PUT /keys/stalled-body HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\n")
        stalled.sendall(b"0123456789")
        received, elapsed = read_until_closed(stalled)

    expect_request_timeout(received, elapsed)
    # 받다 만 body가 연결을 닫을 때 저장되지 않았는지 봅니다.
    status, _, _ = server.request("GET", "/keys/stalled-body")
    expect(status, 404, "멈춘 저장 뒤 조회의 상태 코드")


def idle_after_response_closes_silently(server):
    with server.connect() as idle:
        idle.sendall(b"GET /keys/idle-after-response HTTP/1.1\r\nHost: x\r\n\r\n")
        body = b'{"error":"key_not_found"}'
        response = idle.makefile("rb")
        status_line = response.readline()
        while response.readline() not in (b"\r\n", b""):
            pass
        expect(response.read(len(body)), body, "첫 응답의 body")
        received, elapsed = read_until_closed(idle)

    expect(status_line.split(b" ")[1:2], [b"404"], "첫 응답의 상태 코드")
    expect_not_early(elapsed, "쉬는 연결")
    expect(received, b"", "닫히기 전에 받은 바이트")


def connection_sending_nothing_closes_silently(server):
    # 연결만 열고 요청은 한 바이트도 보내지 않습니다.
    with server.connect() as silent:
        received, elapsed = read_until_closed(silent)

    expect_not_early(elapsed, "아무것도 보내지 않은 연결")
    expect(received, b"", "닫히기 전에 받은 바이트")


def steady_slow_body_is_stored(server):
    chunks = [bytes([index]) * STEADY_BODY_CHUNK for index in range(STEADY_BODY_CHUNKS)]
    value = b"".join(chunks)
    head = f"PUT /keys/steady-body HTTP/1.1\r\nHost: x\r\nContent-Length: {len(value)}\r\n\r\n"
    with server.connect() as steady:
        started = time.monotonic()
        steady.sendall(head.encode("ascii"))
        for chunk in chunks:
            time.sleep(STEADY_BODY_INTERVAL)
            steady.sendall(chunk)
        elapsed = time.monotonic() - started
        status_line = steady.makefile("rb").readline()

    if elapsed <= IDLE_TIMEOUT:
        raise AssertionError(f"body를 {elapsed:.2f}초 만에 다 보내, 기한보다 오래 보내지 못했습니다")
    expect(status_line.split(b" ")[1:2], [b"204"], "천천히 보낸 저장의 상태 코드")
    status, _, body = server.request("GET", "/keys/steady-body")
    expect(status, 200, "천천히 보낸 저장 뒤 조회의 상태 코드")
    if body != value:
        raise AssertionError("조회한 body가 천천히 보낸 body와 다릅니다")


def slow_reader_of_large_response_is_kept(server):
    value = large_value()
    status, _, _ = server.request("PUT", "/keys/slow-read", body=value, timeout=LARGE_REQUEST_TIMEOUT)
    expect(status, 204, "저장의 상태 코드")

    with socket.socket() as reader:
        reader.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, SLOW_READ_BUFFER)
        reader.settimeout(REQUEST_TIMEOUT)
        reader.connect((HOST, server.port))
        reader.sendall(b"GET /keys/slow-read HTTP/1.1\r\nHost: x\r\n\r\n")
        received, elapsed = read_slowly(reader, len(value))

    if elapsed <= IDLE_TIMEOUT:
        raise AssertionError(f"응답을 {elapsed:.2f}초 만에 다 읽어, 기한보다 오래 받지 못했습니다")
    head, _, body = received.partition(HEAD_END)
    expect(head.split(b" ")[1:2], [b"200"], "조회의 상태 코드")
    if body != value:
        raise AssertionError("천천히 받은 body가 저장한 body와 다릅니다")


def read_slowly(reader, body_len):
    """헤더와 body_len바이트의 body를 SLOW_READ_SECONDS에 걸쳐 고르게 읽고,
    받은 바이트와 걸린 초를 돌려줍니다. 다 받기 전에 닫히면 실패합니다."""
    bytes_per_second = body_len / SLOW_READ_SECONDS
    started = time.monotonic()
    received = bytearray()
    while not response_is_complete(received, body_len):
        delay = started + len(received) / bytes_per_second - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        chunk = reader.recv(SLOW_READ_CHUNK)
        if not chunk:
            elapsed = time.monotonic() - started
            raise AssertionError(f"{elapsed:.2f}초에 {len(received)}바이트를 받고 연결이 닫혔습니다")
        received += chunk
    return bytes(received), time.monotonic() - started


def response_is_complete(received, body_len):
    head_len = received.find(HEAD_END)
    return head_len >= 0 and len(received) - head_len - len(HEAD_END) >= body_len


# 하나씩 돌립니다. 작은 GET의 시간을 재므로 다른 테스트와 겹치지 않게 합니다.
TESTS = [
    ("[T14] (기능) 다른 연결이 16 MiB value를 저장하는 중에도 작은 GET은 0.5초 안에 응답을 받는다",
     small_get_prompt_while_storing_large_value),
    ("(기능) 다른 연결이 16 MiB value를 조회해 받아 가는 중에도 작은 GET은 0.5초 안에 응답을 받는다",
     small_get_prompt_while_downloading_large_value),
    ("[T13] (기능) 연결 128개가 열려 있을 때 129번째 연결은 503과 too_many_connections를 받고 닫힌다",
     connection_over_limit_gets_503_and_closed),
    ("(기능) 열린 연결 하나가 닫혀 자리가 나면 새 연결이 다시 정상 응답을 받는다",
     freed_slot_accepts_new_connection),
    ("(기능) 129번째 연결이 거절되는 동안에도 이미 열린 연결의 요청은 정상 응답을 받는다",
     open_connections_served_while_rejecting),
    ("(기능) 413으로 거절한 연결이 함께 보낸 body는 저장되지 않는다",
     body_sent_with_rejected_headers_is_not_stored),
]

# 모두 기한만큼 기다리므로 스레드로 함께 돌립니다.
WAITING_TESTS = [
    ("[T12] (기능) 헤더를 보내다 10초 동안 멈춘 연결은 408과 request_timeout을 받고 닫힌다",
     stalled_headers_get_408_and_close),
    ("(기능) body를 보내다 10초 동안 멈춘 연결은 408과 request_timeout을 받고 닫힌다",
     stalled_body_gets_408_and_close),
    ("(기능) 응답을 받은 뒤 10초 동안 쉬는 연결은 응답 없이 닫힌다", idle_after_response_closes_silently),
    ("(기능) 아무것도 보내지 않은 연결은 10초 뒤 응답 없이 닫힌다", connection_sending_nothing_closes_silently),
    ("(기능) body를 10초보다 오래 걸려 꾸준히 보내는 연결은 끊기지 않고 저장된다", steady_slow_body_is_stored),
    ("(기능) 16 MiB 응답을 10초보다 오래 걸려 천천히 받아 가는 연결은 끊기지 않는다",
     slow_reader_of_large_response_is_kept),
]


def test_run(server, test):
    """테스트를 돌리고, 실패면 그 오류를, 성공이면 None을 돌려줍니다."""
    try:
        test(server)
        return None
    except Exception as error:  # 한 테스트가 실패해도 나머지를 돌립니다.
        return error


def tests_run_one_by_one(server, tests):
    return [test_run(server, test) for _, test in tests]


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
    total = len(TESTS) + len(WAITING_TESTS)
    failed = 0
    try:
        server.wait_until_listening()
        failed += results_print(TESTS, tests_run_one_by_one(server, TESTS))
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
