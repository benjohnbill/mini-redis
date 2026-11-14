#!/usr/bin/env python3
"""측정: sanitizer를 끈 최적화 서버를 띄우고 지연과 처리량을 잽니다(spec Q3).

사용법: python3 -B scripts/bench/bench.py build/bench/mini-redis [시나리오 ...]

`make bench`가 서버를 build/bench/에 만든 뒤 시나리오를 모두 차례로 돌립니다.
하나만 돌릴 때는 이 스크립트에 시나리오 이름을 줍니다. 시나리오마다 새 서버를
띄우고, 끝나면 SIGTERM으로 끕니다.

  lab      key 100,000개를 채운 뒤 작은 GET만 100연결로 보내 p50, p99, 초당
           처리량을 출력합니다(wrk 실험실). 방해가 없을 때의 기준선입니다.
  prefill  key 4,194,304개(2²²)를 채우고 표본 key를 조회해 확인합니다(spec Q9).

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


class Server:
    def __init__(self, binary):
        self.port = free_port()
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen([binary, str(self.port)],
                                        stdout=self.log, stderr=self.log)
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

    def request(self, method, path, body=None):
        connection = http.client.HTTPConnection(HOST, self.port, timeout=REQUEST_TIMEOUT)
        try:
            connection.request(method, path, body=body)
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

def prefill(server, key_count):
    """key prefill-0부터 prefill-<key_count - 1>에 VALUE를 저장하고 표본을 확인합니다.

    열린 루프 시나리오(티켓 02)도 이 함수로 store를 채웁니다."""
    with tempfile.TemporaryDirectory() as marker_dir:
        command = wrk_command("prefill.lua", PREFILL_MAX_SECONDS, server.url,
                              key_count, WRK_THREADS, CONNECTIONS, marker_dir)
        started = time.monotonic()
        wrk = subprocess.Popen(command)
        prefill_wait(wrk, marker_dir)
        elapsed = time.monotonic() - started
    print(f"미리 채우기: key {key_count:,}개를 {elapsed:.1f}초에 저장했습니다(초당 {key_count / elapsed:,.0f}개)",
          flush=True)
    prefill_verify(server, key_count)


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


def prefill_verify(server, key_count):
    """처음, 가운데, 끝과 무작위 key가 VALUE를 돌려주고, 범위 밖 key는 없는지 봅니다."""
    picks = {0, key_count // 2, key_count - 1}
    picks |= {random.randrange(key_count) for _ in range(SAMPLE_COUNT)}
    for index in sorted(picks):
        status, body = server.request("GET", f"/keys/{KEY_PREFIX}{index}")
        if status != 200 or body != VALUE:
            raise BenchError(f"미리 채우기: {KEY_PREFIX}{index} 조회가 {status}, body {len(body)}바이트입니다")
    status, _ = server.request("GET", f"/keys/{KEY_PREFIX}{key_count}")
    if status != 404:
        raise BenchError(f"미리 채우기: 범위 밖 {KEY_PREFIX}{key_count} 조회가 {status}입니다")
    print(f"미리 채우기: 표본 key {len(picks)}개가 들어 있고, 범위 밖 key는 없습니다", flush=True)


# ── 시나리오 ──────────────────────────────────────────────────

def lab(server):
    prefill(server, LAB_KEY_COUNT)
    command = wrk_command("lab_get.lua", LAB_SECONDS, server.url, LAB_KEY_COUNT)
    command[1:1] = ["--latency"]
    if subprocess.run(command).returncode != 0:
        raise BenchError("실험실: wrk가 오류를 보고했습니다")


def rehash_prefill(server):
    prefill(server, REHASH_KEY_COUNT)


SCENARIOS = {
    "lab": lab,
    "prefill": rehash_prefill,
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
