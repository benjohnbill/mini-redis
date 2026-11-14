"""열린 루프 client: 요청을 정해진 시각마다 보내고 요청 종류별 지연을 잽니다(spec Q3).

닫힌 루프(wrk)는 응답을 받아야 다음 요청을 보내므로, 서버가 멈춘 동안에는 요청을
보내지 않고 그 기다림이 숫자에서 빠집니다. 여기서는 요청마다 "보냈어야 할 시각"을
먼저 정하고, 지연을 응답을 다 받은 시각에서 그 예정 시각을 뺀 값으로 잽니다.
연결이 앞 요청의 응답을 기다리는 중이면 새 요청은 그 연결의 줄에서 기다렸다가
보내고, 그 기다림도 지연에 들어갑니다.

python3 표준 라이브러리만 씁니다. 스레드 하나가 selectors(epoll)로 모든 연결을
돌립니다. epoll의 기다림은 ms 단위로 올림되어 예정 시각보다 늦게 깨므로, 다음 예정
시각이 SPIN_NS 안으로 들어오면 기다리지 않고 계속 확인합니다(CPU 코어 하나를
씁니다). client가 늦게 보낸 정도는 따로 재어(client 늦음) 숫자를 믿을 수 있는지
봅니다.
"""

import collections
import gc
import math
import selectors
import socket
import time

HOST = "127.0.0.1"
SPIN_NS = 2_000_000  # 다음 예정 시각이 이만큼 남으면 epoll로 기다리지 않습니다
SEND_CHUNK = 256 * 1024  # 큰 body를 나눠 보내 작은 GET의 송신을 오래 막지 않습니다
RECV_SIZE = 65536
HEAD_END = b"\r\n\r\n"  # 헤더의 끝입니다. 마지막 헤더 줄의 줄바꿈과 빈 줄입니다
DRAIN_TIMEOUT_NS = 60_000_000_000  # 시간표가 끝난 뒤 남은 응답을 기다리는 한도
# 시간표가 끝나 보낼 요청이 없을 때, 남은 응답을 기다리며 epoll이 한 번에 쉬는 시간
DRAIN_WAIT_SECONDS = 0.1
NS_PER_MS = 1_000_000
NS_PER_SECOND = 1_000_000_000
CLIENT_LATE = "client 늦음"


class OpenLoopError(Exception):
    pass


class Request:
    """보낼 요청 하나와 기대하는 응답입니다. kind는 지연을 모으는 이름입니다."""

    def __init__(self, kind, method, path, body=None, expect_status=200,
                 expect_body=None, headers=(), expect_json=False):
        self.kind = kind
        self.method = method
        self.path = path
        self.body = body
        self.expect_status = expect_status
        self.expect_body = expect_body
        self.headers = headers  # (이름, 값) 쌍들, 예: (("TTL", "2"),)
        self.expect_json = expect_json  # body가 JSON 객체로 시작하는지 확인합니다
        self.scheduled_ns = 0
        self.sent_ns = 0  # 첫 바이트를 보내기 시작한 시각(perf_counter_ns)

    def chunks(self):
        head = f"{self.method} {self.path} HTTP/1.1\r\nHost: bench\r\n"
        for name, text in self.headers:
            head += f"{name}: {text}\r\n"
        if self.body is not None:
            head += f"Content-Length: {len(self.body)}\r\n"
        parts = [memoryview((head + "\r\n").encode())]
        if self.body:
            parts.append(memoryview(self.body))
        return parts


class Stream:
    """요청을 interval_ns마다 하나씩 내고, 연결들에 차례로 맡깁니다.

    next_request는 몇 번째 요청인지 받아 Request를 돌려주고, 더 보낼 것이 없으면
    None을 돌려줍니다."""

    def __init__(self, connections, interval_ns, next_request):
        self.connections = connections
        self.interval_ns = interval_ns
        self.next_request = next_request
        self.index = 0
        self.next_ns = 0
        self.is_finished = False


class Connection:
    """서버에 연 연결 하나입니다. 소켓은 기다리지 않는(non-blocking) 상태로 둡니다."""

    def __init__(self, port):
        self.socket = socket.create_connection((HOST, port))
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.socket.setblocking(False)
        self.waiting = collections.deque()
        self.current = None
        self.outgoing = collections.deque()
        self.received = bytearray()
        self.is_writing = False


class Run:
    """streams를 duration_ns 동안 돌리고 요청 종류별 지연(ns) 목록을 모읍니다.

    strict가 거짓이면 기대와 다른 응답이나 끝나지 않은 요청을 오류·timeout으로
    세고 계속합니다. 캐시 실험은 503과 timeout도 결과로 남겨야 하기 때문입니다."""

    def __init__(self, port, duration_ns, strict=True):
        self.port = port
        self.strict = strict
        self.errors = collections.Counter()
        self.timeouts = 0
        self.duration_ns = duration_ns
        self.selector = selectors.DefaultSelector()
        self.connections = []
        self.streams = []
        self.latencies = collections.defaultdict(list)
        self.pending = 0

    def connections_open(self, count):
        opened = []
        for _ in range(count):
            connection = Connection(self.port)
            self.selector.register(connection.socket, selectors.EVENT_READ, connection)
            self.connections.append(connection)
            opened.append(connection)
        return opened

    def stream_add(self, connections, interval_ns, next_request):
        self.streams.append(Stream(connections, interval_ns, next_request))

    def run(self):
        gc.disable()
        try:
            self.loop()
        finally:
            gc.enable()
            for connection in self.connections:
                self.selector.unregister(connection.socket)
                connection.socket.close()
            self.selector.close()
        return self.latencies

    def loop(self):
        started = time.perf_counter_ns()
        end = started + self.duration_ns
        for stream in self.streams:
            stream.next_ns = started
        while True:
            now = time.perf_counter_ns()
            next_ns = self.streams_release(now, end)
            if next_ns is None and self.pending == 0:
                return
            if next_ns is None and now - end > DRAIN_TIMEOUT_NS:
                if self.strict:
                    raise OpenLoopError(f"시간표가 끝난 뒤 응답 {self.pending}개가 오지 않습니다")
                self.timeouts = self.pending
                self.pending = 0
                return
            self.events_handle(self.wait_seconds(now, next_ns))

    def streams_release(self, now, end):
        """예정 시각이 지난 요청을 연결에 맡기고, 다음 예정 시각을 돌려줍니다."""
        next_ns = None
        for stream in self.streams:
            while not stream.is_finished and stream.next_ns <= now:
                self.stream_release_one(stream, end)
            if not stream.is_finished:
                next_ns = stream.next_ns if next_ns is None else min(next_ns, stream.next_ns)
        return next_ns

    def stream_release_one(self, stream, end):
        request = None if stream.next_ns >= end else stream.next_request(stream.index)
        if request is None:
            stream.is_finished = True
            return
        request.scheduled_ns = stream.next_ns
        connection = stream.connections[stream.index % len(stream.connections)]
        stream.index += 1
        stream.next_ns += stream.interval_ns
        self.pending += 1
        if connection.current is None:
            self.request_start(connection, request, is_on_time=True)
        else:
            connection.waiting.append(request)

    def wait_seconds(self, now, next_ns):
        if next_ns is None:
            return DRAIN_WAIT_SECONDS
        left = next_ns - now - SPIN_NS
        return left / NS_PER_SECOND if left > 0 else 0

    def request_start(self, connection, request, is_on_time):
        connection.current = request
        request.sent_ns = time.perf_counter_ns()
        connection.outgoing.extend(request.chunks())
        self.outgoing_send(connection)
        # 연결이 비어 있어 예정 시각에 바로 보낼 수 있었던 요청만, client가
        # 늦게 보낸 정도를 잽니다. 서버의 응답을 기다린 시간은 넣지 않습니다.
        if is_on_time:
            self.latencies[CLIENT_LATE].append(time.perf_counter_ns() - request.scheduled_ns)

    def outgoing_send(self, connection):
        while connection.outgoing:
            chunk = connection.outgoing[0]
            try:
                sent = connection.socket.send(chunk[:SEND_CHUNK])
            except BlockingIOError:
                break
            if sent == len(chunk):
                connection.outgoing.popleft()
            else:
                connection.outgoing[0] = chunk[sent:]
                break
        self.write_interest_set(connection, bool(connection.outgoing))

    def write_interest_set(self, connection, is_writing):
        if connection.is_writing == is_writing:
            return
        connection.is_writing = is_writing
        events = selectors.EVENT_READ | (selectors.EVENT_WRITE if is_writing else 0)
        self.selector.modify(connection.socket, events, connection)

    def events_handle(self, timeout):
        for key, mask in self.selector.select(timeout):
            connection = key.data
            if mask & selectors.EVENT_WRITE:
                self.outgoing_send(connection)
            if mask & selectors.EVENT_READ:
                self.response_receive(connection)

    def response_receive(self, connection):
        data = connection.socket.recv(RECV_SIZE)
        now = time.perf_counter_ns()
        if not data:
            raise OpenLoopError("서버가 연결을 닫았습니다")
        connection.received += data
        response = response_parse(connection.received)
        if response is None:
            return
        status, body, used = response
        del connection.received[:used]
        request = connection.current
        if request is None:
            raise OpenLoopError(f"보내지 않은 요청의 응답이 왔습니다: {status}")
        try:
            response_check(request, status, body)
        except OpenLoopError:
            if self.strict:
                raise
            self.errors[request.kind] += 1
        self.latencies[request.kind].append(now - request.scheduled_ns)
        self.pending -= 1
        connection.current = None
        if connection.waiting:
            self.request_start(connection, connection.waiting.popleft(), is_on_time=False)


def response_parse(received):
    """응답 하나가 다 왔으면 (상태 코드, body, 쓴 바이트 수), 아니면 None입니다."""
    head_len = received.find(HEAD_END)
    if head_len < 0:
        return None
    lines = bytes(received[:head_len]).decode("latin-1").split("\r\n")
    status = int(lines[0].split()[1])
    body_len = 0
    for line in lines[1:]:
        name, _, value = line.partition(":")
        if name.lower() == "content-length":
            body_len = int(value)
    body_start = head_len + len(HEAD_END)
    if len(received) < body_start + body_len:
        return None
    return status, bytes(received[body_start:body_start + body_len]), body_start + body_len


def response_check(request, status, body):
    if status != request.expect_status or (
            request.expect_body is not None and body != request.expect_body):
        raise OpenLoopError(f"{request.method} {request.path}: 기대 {request.expect_status}, "
                            f"받은 {status}, body {len(body)}바이트")
    if request.expect_json and not body.startswith(b"{"):
        raise OpenLoopError(f"{request.method} {request.path}: JSON body가 아닙니다")


def percentile(sorted_values, percent):
    """가장 가까운 순위(nearest-rank)의 백분위수입니다."""
    rank = max(1, math.ceil(percent / 100 * len(sorted_values)))
    return sorted_values[rank - 1]


def summary_line(kind, latencies):
    values = sorted(latencies)
    return (f"{kind}: {len(values):,}개, p50 {percentile(values, 50) / NS_PER_MS:.3f} ms, "
            f"p99 {percentile(values, 99) / NS_PER_MS:.3f} ms, "
            f"max {values[-1] / NS_PER_MS:.3f} ms")
