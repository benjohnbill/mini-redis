#!/usr/bin/env python3
"""측정: sanitizer를 끈 최적화 서버를 띄우고 지연과 처리량을 잽니다(spec Q3).

사용법: python3 -B scripts/bench/bench.py build/bench/mini-redis [시나리오 ...]

`make bench`가 서버를 build/bench/에 만든 뒤 시나리오를 모두 차례로 돌립니다.
하나만 돌릴 때는 이 스크립트에 시나리오 이름을 줍니다. 시나리오마다 새 서버를
띄우고, 끝나면 SIGTERM으로 끕니다.

  lab      key 100,000개를 채운 뒤 작은 GET만 100연결로 보내 p50, p99, 초당
           처리량을 출력합니다(wrk 실험실). 방해가 없을 때의 기준선입니다.

아래 다섯 시나리오는 python 열린 루프(open_loop.py)로 작은 GET을 100연결에 걸쳐 초당
SMALL_GET_RATE개씩 정해진 시각마다 보내고, 작은 GET과 방해 요청의 지연(예정 시각부터
응답까지)을 따로 출력합니다(spec Q3, 티켓 02).

  a-quiet       방해 없음. key 100,000개. python 기준선을 wrk 실험실과 견줍니다.
  b-big-store   16 MiB value를 새 key에 저장하는 요청을 LARGE_INTERVAL마다 보냅니다.
  c-rehash      key 4,194,304개(2²²)를 채우고 하나를 지운 뒤, 새 key 저장과 삭제를
                REHASH_INTERVAL마다 번갈아 보냅니다. 요청마다 rehash가 일어납니다(Q9).
  d-big-delete  16 MiB value를 미리 넣어 둔 key를 LARGE_INTERVAL마다 하나씩 지웁니다.
  e-grow-once   key 4,194,304개를 채우고 버킷을 늘리는 저장 한 번을 보냅니다.

아래 세 시나리오는 M4 expire의 주기 청소를 잽니다(M4 spec Q20). 방해 요청 대신
만료되는 key를 두고, 작은 GET의 지연과 함께 회수 지연(만료된 key가 만료 시각 뒤에
남은 시간)을 출력합니다. 회수 지연은 서버가 SWEEP_TRACE 빌드에서 stderr에 적는
청소 기록("sweep <mg_millis> <지운 개수>")으로 잽니다(make bench가 켭니다).

  f-expire-burst   key 100,000개를 TTL EXPIRE_TTL초로 한꺼번에 저장해, 측정 중에
                   거의 같은 순간에 만료되게 합니다(한꺼번에 만료).
  g-expire-steady  측정 동안 key 하나를 TTL STEADY_TTL초로 1 ms마다 저장해, 초당
                   1,000개씩 만료되게 합니다(꾸준한 만료).
  h-expire-large   16 MiB value 50개를 TTL EXPIRE_TTL초로 이어서 저장해, 측정 중에
                   함께 만료되게 합니다(큰 value의 만료).

새 시나리오는 SCENARIOS에 함수를 더합니다. 함수는 띄운 서버 하나를 받습니다.
측정은 make verify와 CI에 넣지 않습니다. wrk는 측정하는 팀원에게만 필요합니다.
"""

import http.client
import os
import random
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

import open_loop

HOST = "127.0.0.1"
START_TIMEOUT = 10.0
STOP_TIMEOUT = 30.0  # 4,194,304개를 담은 store를 해제하는 시간까지 기다립니다
REQUEST_TIMEOUT = 5.0
WRK_INSTALL = "sudo apt install wrk"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# prefill.lua와 같은 key 이름과 value입니다.
KEY_PREFIX = "prefill-"
VALUE = b"v" * 100

CONNECTIONS = 100  # 기준 동시 연결 수, spec Q2
WRK_THREADS = 4
LAB_KEY_COUNT = 100_000  # "Redis와 대조"의 방해 없음 측정과 같은 key 수
LAB_SECONDS = 10
REHASH_KEY_COUNT = 4_194_304  # 2²², spec Q9
PREFILL_MAX_SECONDS = 600  # 미리 채우기가 이 시간 안에 끝나지 않으면 실패입니다
PREFILL_POLL_INTERVAL = 0.2
SAMPLE_COUNT = 8  # 미리 채운 뒤 무작위로 골라 조회하는 key 수

# 열린 루프 시나리오(티켓 02)
NS_PER_SECOND = 1_000_000_000
OPEN_LOOP_SECONDS = 10
SMALL_GET_RATE = 5_000  # 초당 작은 GET 수. 연결 하나는 20 ms마다 하나를 보냅니다
LARGE_VALUE = b"b" * 16_777_216  # 상한 크기 value, API_BODY_MAX
LARGE_INTERVAL = 0.2  # 초. 16 MiB 저장과 삭제를 이 간격으로 보냅니다
LARGE_KEY_COUNT = round(OPEN_LOOP_SECONDS / LARGE_INTERVAL)  # 50개, 합쳐서 800 MiB
REHASH_INTERVAL = 1.0  # 초. rehash 하나(약 0.3초)가 끝나고 서버가 쉬는 틈을 둡니다
GROW_DELAY = OPEN_LOOP_SECONDS / 2  # 초. 버킷을 늘리는 저장 한 번을 보내는 간격입니다
SMALL_GET = "작은 GET"

# 주기 청소 측정(M4 expire)
MS_PER_SECOND = 1000
NS_PER_MS = 1_000_000
EXPIRE_TTL = 4  # 초. 미리 저장한 key가 측정을 시작하고 2~3초 뒤에 만료됩니다
EXPIRE_BURST_COUNT = 100_000  # 한꺼번에 만료되는 key 수, M4 spec Q20
BURST_PREFIX = "burst-"
LAST_KEY = "/keys/expire-last"
STEADY_TTL = 2  # 초. 측정 2초부터 끝난 뒤 2초까지 만료됩니다
STEADY_INTERVAL = 0.001  # 초. 초당 1,000개, M4 spec Q20
RECOVERY_MAX_SECONDS = 30  # 열린 루프가 끝난 뒤 청소가 다 지우기를 기다리는 한도
RECOVERY_POLL_INTERVAL = 0.1
SWEEP_TRACE_PREFIX = "sweep "


class Server:
    def __init__(self, binary):
        self.port = free_port()
        # mongoose의 로그는 버퍼를 거치는 stdout으로 나가서 줄 중간에서 끊겨 써질 수
        # 있습니다. 청소 기록 줄이 그 사이에 끼어 깨지지 않게, 서버가 직접 쓰는
        # stderr를 다른 파일로 받습니다.
        self.log = tempfile.TemporaryFile()
        self.errors = tempfile.TemporaryFile()
        self.process = subprocess.Popen([binary, str(self.port)],
                                        stdout=self.log, stderr=self.errors)
        self.url = f"http://{HOST}:{self.port}"

    def wait_until_listening(self):
        deadline = time.monotonic() + START_TIMEOUT
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise BenchError(f"서버가 시작 중에 끝났습니다: 종료 코드 {self.process.returncode}\n{self.output()}")
            try:
                socket.create_connection((HOST, self.port), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.02)
        raise BenchError(f"서버가 {START_TIMEOUT}초 안에 연결을 받지 않습니다")

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
        self.errors.seek(0)
        self.log.seek(0)
        return (self.errors.read() + self.log.read()).decode("utf-8", "replace")

    def sweeps_read(self):
        """SWEEP_TRACE 빌드가 적은 청소 기록을 (mg_millis, 지운 개수) 목록으로 읽습니다.

        서버가 아직 같은 파일에 쓰는 중이므로, 파일의 위치를 옮기지 않는 pread로
        읽습니다. seek하면 서버가 쓰는 위치도 함께 옮겨집니다."""
        fd = self.errors.fileno()
        text = os.pread(fd, os.fstat(fd).st_size, 0).decode("utf-8", "replace")
        sweeps = []
        for line in text.splitlines():
            if line.startswith(SWEEP_TRACE_PREFIX):
                millis, count = line[len(SWEEP_TRACE_PREFIX):].split()
                sweeps.append((int(millis), int(count)))
        return sweeps


class BenchError(Exception):
    pass


def free_port():
    with socket.socket() as probe:
        probe.bind((HOST, 0))
        return probe.getsockname()[1]


def wrk_require():
    if shutil.which("wrk") is None:
        raise BenchError(f"wrk가 없습니다. 설치한 뒤 다시 실행하세요: {WRK_INSTALL}")


def wrk_command(lua_name, seconds, url, *args):
    command = ["wrk", f"-t{WRK_THREADS}", f"-c{CONNECTIONS}", f"-d{seconds}s",
               "-s", os.path.join(SCRIPT_DIR, lua_name), url]
    if args:
        command += ["--", *map(str, args)]
    print("$ " + " ".join(command), flush=True)
    return command


# ── 미리 채우기 ───────────────────────────────────────────────

def prefill(server, key_count, prefix=KEY_PREFIX, ttl=None):
    """key <prefix>0부터 <prefix><key_count - 1>에 VALUE를 저장하고 표본을 확인합니다.

    열린 루프 시나리오(티켓 02)도 이 함수로 store를 채웁니다. ttl(초)을 주면
    저장마다 TTL 헤더를 붙입니다."""
    with tempfile.TemporaryDirectory() as marker_dir:
        options = [prefix] + ([ttl] if ttl is not None else [])
        command = wrk_command("prefill.lua", PREFILL_MAX_SECONDS, server.url,
                              key_count, WRK_THREADS, CONNECTIONS, marker_dir, *options)
        started = time.monotonic()
        wrk = subprocess.Popen(command)
        prefill_wait(wrk, marker_dir)
        elapsed = time.monotonic() - started
    print(f"미리 채우기: key {key_count:,}개를 {elapsed:.1f}초에 저장했습니다(초당 {key_count / elapsed:,.0f}개)",
          flush=True)
    prefill_verify(server, key_count, prefix)


def prefill_wait(wrk, marker_dir):
    """스레드가 모두 done 표시를 남기면 wrk에 SIGINT를 보내 결과를 출력하게 합니다."""
    while wrk.poll() is None:
        markers = os.listdir(marker_dir)
        failed = [name for name in markers if name.startswith("failed-")]
        done = [name for name in markers if name.startswith("done-")]
        if failed or len(done) == WRK_THREADS:
            break
        time.sleep(PREFILL_POLL_INTERVAL)
    if wrk.poll() is None:
        wrk.send_signal(signal.SIGINT)
    if wrk.wait() != 0:
        raise BenchError("미리 채우기: wrk가 오류를 보고했습니다")
    markers = os.listdir(marker_dir)
    if any(name.startswith("failed-") for name in markers):
        raise BenchError("미리 채우기: 저장이나 조회가 오류 응답을 받았습니다")
    if sum(name.startswith("done-") for name in markers) != WRK_THREADS:
        raise BenchError(f"미리 채우기: {PREFILL_MAX_SECONDS}초 안에 끝나지 않았습니다")


def prefill_verify(server, key_count, prefix):
    """처음, 가운데, 끝과 무작위 key가 VALUE를 돌려주고, 범위 밖 key는 없는지 봅니다."""
    picks = {0, key_count // 2, key_count - 1}
    picks |= {random.randrange(key_count) for _ in range(SAMPLE_COUNT)}
    for index in sorted(picks):
        status, body = server.request("GET", f"/keys/{prefix}{index}")
        if status != 200 or body != VALUE:
            raise BenchError(f"미리 채우기: {prefix}{index} 조회가 {status}, body {len(body)}바이트입니다")
    status, _ = server.request("GET", f"/keys/{prefix}{key_count}")
    if status != 404:
        raise BenchError(f"미리 채우기: 범위 밖 {prefix}{key_count} 조회가 {status}입니다")
    print(f"미리 채우기: 표본 key {len(picks)}개가 들어 있고, 범위 밖 key는 없습니다", flush=True)


# ── 시나리오 ──────────────────────────────────────────────────

def lab(server):
    prefill(server, LAB_KEY_COUNT)
    command = wrk_command("lab_get.lua", LAB_SECONDS, server.url, LAB_KEY_COUNT)
    command[1:1] = ["--latency"]
    if subprocess.run(command).returncode != 0:
        raise BenchError("실험실: wrk가 오류를 보고했습니다")


def quiet(server):
    prefill(server, LAB_KEY_COUNT)
    open_loop_measure(server, LAB_KEY_COUNT)


def large_store(server):
    prefill(server, LAB_KEY_COUNT)

    def large_store_request(index):
        return open_loop.Request("16 MiB 저장", "PUT", f"/keys/large-{index}",
                                 LARGE_VALUE, expect_status=204)

    open_loop_measure(server, LAB_KEY_COUNT, LARGE_INTERVAL, large_store_request)


def rehash(server):
    """M3 전의 hash.c는 넣고 지울 때마다 버킷 수를 "entry 수 ÷ 2보다 작거나 같은
    가장 큰 2의 거듭제곱"으로 다시 맞췄습니다. entry가 2²²개면 버킷 2²¹개, 2²² - 1개면
    2²⁰개라서, 하나를 지워 2²² - 1개로 만든 뒤 새 key를 저장하고 지우기를 번갈아 하면
    요청마다 entry 전부를 옮겼습니다. 지금의 hash.c는 버킷당 entry가 1~4개인 동안
    버킷 수를 바꾸지 않으므로, 이 시나리오는 그 반복이 다시 생기지 않는지 봅니다."""
    prefill(server, REHASH_KEY_COUNT)
    last_key = f"/keys/{KEY_PREFIX}{REHASH_KEY_COUNT - 1}"
    status, _ = server.request("DELETE", last_key)
    if status != 204:
        raise BenchError(f"rehash: {last_key} 삭제가 {status}입니다")

    def rehash_request(index):
        if index % 2 == 0:
            return open_loop.Request("rehash 저장", "PUT", "/keys/rehash-new",
                                     VALUE, expect_status=204)
        return open_loop.Request("rehash 삭제", "DELETE", "/keys/rehash-new",
                                 expect_status=204)

    open_loop_measure(server, REHASH_KEY_COUNT - 1, REHASH_INTERVAL, rehash_request)


def grow_once(server):
    """지금의 hash.c는 버킷당 entry가 4개를 넘을 때 버킷을 늘립니다. 2²²개를 채우면
    버킷 2²⁰개에 entry가 꼭 4개씩이라서, 새 key 하나를 저장하는 순간 버킷이 2²¹개로
    늘며 entry 전부를 한 번 옮깁니다. key 수가 2배로 늘 때마다 한 번 생기는 이 멈춤을
    봅니다. 방해 요청은 측정 동안 저장 한 번뿐입니다."""
    prefill(server, REHASH_KEY_COUNT)

    def grow_request(index):
        if index > 0:
            return None
        return open_loop.Request("버킷을 늘리는 저장", "PUT", "/keys/grow-new",
                                 VALUE, expect_status=204)

    open_loop_measure(server, REHASH_KEY_COUNT, GROW_DELAY, grow_request)


def large_delete(server):
    prefill(server, LAB_KEY_COUNT)
    for index in range(LARGE_KEY_COUNT):
        status, _ = server.request("PUT", f"/keys/large-{index}", LARGE_VALUE)
        if status != 204:
            raise BenchError(f"16 MiB 삭제: large-{index} 저장이 {status}입니다")
    print(f"16 MiB value {LARGE_KEY_COUNT}개를 저장했습니다", flush=True)

    def large_delete_request(index):
        if index >= LARGE_KEY_COUNT:
            return None
        return open_loop.Request("16 MiB 삭제", "DELETE", f"/keys/large-{index}",
                                 expect_status=204)

    open_loop_measure(server, LAB_KEY_COUNT, LARGE_INTERVAL, large_delete_request)


def expire_burst(server):
    prefill(server, LAB_KEY_COUNT)
    batch_started = server_millis()
    prefill(server, EXPIRE_BURST_COUNT, BURST_PREFIX, EXPIRE_TTL)
    expiring_batch_measure(server, EXPIRE_BURST_COUNT, batch_started)


def expire_steady(server):
    prefill(server, LAB_KEY_COUNT)
    stored = []

    def steady_store_request(index):
        request = open_loop.Request("TTL 저장", "PUT", f"/keys/steady-{index}", VALUE,
                                    expect_status=204, headers=(("TTL", str(STEADY_TTL)),))
        stored.append(request)
        return request

    open_loop_measure(server, LAB_KEY_COUNT, STEADY_INTERVAL, steady_store_request)
    # 저장마다 보내기 시작한 시각(perf_counter, CLOCK_MONOTONIC)을 서버의 시계
    # (mg_millis, CLOCK_MONOTONIC_RAW)로 옮깁니다. 서버는 그 뒤에 헤더를 받으므로,
    # 이 만료 시각은 실제보다 편도 전송 지연만큼 이르고 회수 지연은 그만큼 길게 잽니다.
    offset_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW) - time.perf_counter_ns()
    expiries = sorted((request.sent_ns + offset_ns) / NS_PER_MS + STEADY_TTL * MS_PER_SECOND
                      for request in stored)
    deleted_at = recovery_wait(server, len(expiries))
    if len(deleted_at) < len(expiries):
        print(f"회수 지연: key {len(expiries):,}개 가운데 {len(deleted_at):,}개만 "
              f"{RECOVERY_MAX_SECONDS}초 안에 지웠습니다", flush=True)
        expiries = expiries[:len(deleted_at)]
    # heap은 만료 시각 순으로 꺼내므로, i번째로 지운 key는 i번째로 만료된 key입니다.
    delays = sorted(deleted - expiry for deleted, expiry in zip(deleted_at, expiries))
    print(f"회수 지연: key {len(delays):,}개, p50 {open_loop.percentile(delays, 50):.1f} ms, "
          f"p99 {open_loop.percentile(delays, 99):.1f} ms, max {delays[-1]:.1f} ms", flush=True)


def expire_large(server):
    prefill(server, LAB_KEY_COUNT)
    batch_started = server_millis()
    for index in range(LARGE_KEY_COUNT):
        ttl_store(server, f"/keys/large-expire-{index}", LARGE_VALUE)
    print(f"16 MiB value {LARGE_KEY_COUNT}개를 TTL {EXPIRE_TTL}초로 저장했습니다", flush=True)
    expiring_batch_measure(server, LARGE_KEY_COUNT, batch_started)


def expiring_batch_measure(server, key_count, batch_started):
    """미리 저장한 key key_count개 뒤에 같은 TTL로 LAST_KEY를 저장하고, 열린 루프로
    잰 뒤 청소가 모두 지우기를 기다립니다.

    LAST_KEY는 맨 나중에 저장해 만료 시각이 가장 늦으므로 청소가 마지막에 지웁니다.
    청소는 저장보다 느리게 지우므로, 만료 시각 뒤에 가장 오래 남는 key도 LAST_KEY입니다.
    그래서 LAST_KEY를 지운 시각에서 그 만료 시각을 뺀 값을 묶음의 회수 지연으로 봅니다.
    보내기 전의 시각으로 만료 시각을 정하므로, 편도 전송 지연만큼 길게 잽니다."""
    last_expiry = server_millis() + EXPIRE_TTL * MS_PER_SECOND
    ttl_store(server, LAST_KEY, VALUE)
    if server_millis() >= batch_started + EXPIRE_TTL * MS_PER_SECOND:
        raise BenchError(f"저장이 TTL {EXPIRE_TTL}초보다 오래 걸려, 측정 전에 만료가 시작됐습니다")
    open_loop_measure(server, LAB_KEY_COUNT)
    deleted_at = recovery_wait(server, key_count + 1)
    if len(deleted_at) <= key_count:
        print(f"회수 지연: key {key_count + 1:,}개 가운데 {len(deleted_at):,}개만 지웠고, "
              f"마지막 key는 {server_millis() - last_expiry:,} ms 넘게 남았습니다", flush=True)
        return
    print(f"회수 지연: 마지막 key가 만료 시각 뒤 {deleted_at[key_count] - last_expiry:,} ms 남았습니다"
          f"(key {key_count + 1:,}개)", flush=True)


def ttl_store(server, path, value):
    status, _ = server.request("PUT", path, value, {"TTL": str(EXPIRE_TTL)})
    if status != 204:
        raise BenchError(f"{path} 저장이 {status}입니다")


def server_millis():
    """서버의 mg_millis와 같은 시계(CLOCK_MONOTONIC_RAW)의 지금 ms입니다."""
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW) // NS_PER_MS


def recovery_wait(server, key_count):
    """청소가 key key_count개를 지우거나 RECOVERY_MAX_SECONDS가 지날 때까지 기다리고,
    지운 key마다 지운 시각(mg_millis)을 지운 순서대로 돌려줍니다."""
    deadline = time.monotonic() + RECOVERY_MAX_SECONDS
    while True:
        sweeps = server.sweeps_read()
        deleted_count = sum(count for _, count in sweeps)
        if deleted_count >= key_count or time.monotonic() >= deadline:
            break
        time.sleep(RECOVERY_POLL_INTERVAL)
    if not sweeps:
        raise BenchError("청소 기록이 없습니다. 서버를 -DSWEEP_TRACE로 빌드했는지 확인하세요")
    largest = max(count for _, count in sweeps)
    print(f"청소: {len(sweeps):,}번이 key {deleted_count:,}개를 지웠고, 한 번에 최대 {largest:,}개",
          flush=True)

    return [millis for millis, count in sweeps for _ in range(count)]


def open_loop_measure(server, key_count, disturb_interval=None, disturb_request=None):
    """작은 GET을 CONNECTIONS개 연결에 걸쳐 초당 SMALL_GET_RATE개씩 보내고, 방해
    요청이 있으면 연결 하나로 disturb_interval초마다 보냅니다. 끝나면 요청 종류별로
    p50, p99, max를 출력합니다."""
    run = open_loop.Run(server.port, OPEN_LOOP_SECONDS * NS_PER_SECOND)
    picker = random.Random(key_count)

    def small_get_request(index):
        path = f"/keys/{KEY_PREFIX}{picker.randrange(key_count)}"
        return open_loop.Request(SMALL_GET, "GET", path, expect_body=VALUE)

    run.stream_add(run.connections_open(CONNECTIONS), NS_PER_SECOND // SMALL_GET_RATE,
                   small_get_request)
    if disturb_request is not None:
        run.stream_add(run.connections_open(1), round(disturb_interval * NS_PER_SECOND),
                       disturb_request)
    print(f"열린 루프: 작은 GET 초당 {SMALL_GET_RATE:,}개를 {CONNECTIONS}연결로 "
          f"{OPEN_LOOP_SECONDS}초 동안 보냅니다", flush=True)
    try:
        latencies = run.run()
    except (OSError, open_loop.OpenLoopError) as error:
        raise BenchError(f"열린 루프: {error}") from error
    kinds = [SMALL_GET] + sorted(set(latencies) - {SMALL_GET, open_loop.CLIENT_LATE})
    for kind in kinds + [open_loop.CLIENT_LATE]:
        print(open_loop.summary_line(kind, latencies[kind]), flush=True)


SCENARIOS = {
    "lab": lab,
    "a-quiet": quiet,
    "b-big-store": large_store,
    "c-rehash": rehash,
    "d-big-delete": large_delete,
    "e-grow-once": grow_once,
    "f-expire-burst": expire_burst,
    "g-expire-steady": expire_steady,
    "h-expire-large": expire_large,
}


def scenario_run(binary, name):
    print(f"\n== {name}", flush=True)
    server = Server(binary)
    try:
        server.wait_until_listening()
        SCENARIOS[name](server)
    finally:
        exit_code = server.stop()
    if exit_code != 0:
        raise BenchError(f"서버가 종료 코드 {exit_code}로 끝났습니다\n{server.output()}")


def main():
    if len(sys.argv) < 2 or any(name not in SCENARIOS for name in sys.argv[2:]):
        print(f"usage: {sys.argv[0]} <server> [{' | '.join(SCENARIOS)} ...]", file=sys.stderr)
        return 2
    binary = sys.argv[1]
    names = sys.argv[2:] or list(SCENARIOS)
    try:
        wrk_require()
        for name in names:
            scenario_run(binary, name)
    except BenchError as error:
        print(f"실패: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
