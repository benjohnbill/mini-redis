#!/usr/bin/env python3
"""측정: 요청이 없는 서버가 1초에 몇 번 깨어나는지 잽니다(M7 perf).

사용법: python3 -B scripts/bench/idle_wakeup.py build/mini-redis [초]

서버를 띄우고 아래 상태마다 [초](기본 5초) 동안 서버 프로세스의 문맥 전환 수를
/proc/<pid>/status에서 읽어 그 차이를 셉니다. 서버는 스레드 하나라서, 잠들었다가
깨어날 때마다 문맥 전환이 한 번 늘어납니다. 상태마다 REPEAT번 재고 초당 횟수를
출력합니다. /proc를 읽으므로 Linux에서만 돕니다.

  empty        key도 연결도 없습니다.
  ttl-key      보관 기간 1시간인 key가 하나 있고, 연결은 없습니다.
  idle-conn    key가 없고, 아무것도 보내지 않는 연결이 하나 열려 있습니다.
               서버의 쉬는 시간 기한(10초) 안에 재도록 [초]는 9 이하여야 합니다.
"""

import http.client
import socket
import subprocess
import sys
import time

HOST = "127.0.0.1"
START_TIMEOUT = 10.0
DEFAULT_SECONDS = 5.0
REPEAT = 3
SETTLE_SECONDS = 0.5  # 상태를 만든 요청의 처리가 끝나기를 기다립니다
IDLE_TIMEOUT = 10.0  # 서버의 쉬는 시간 기한(M3 spec Q4)
TTL_SECONDS = 3600


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    binary = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) == 3 else DEFAULT_SECONDS
    if seconds + SETTLE_SECONDS >= IDLE_TIMEOUT:
        sys.exit(f"[초]는 {IDLE_TIMEOUT - SETTLE_SECONDS}초보다 짧아야 합니다")
    for name, prepare in (("empty", prepare_empty),
                          ("ttl-key", prepare_ttl_key),
                          ("idle-conn", prepare_idle_connection)):
        rates = [measure_once(binary, prepare, seconds) for _ in range(REPEAT)]
        shown = ", ".join(f"{rate:.1f}" for rate in rates)
        print(f"{name:10} 초당 깨어남: {shown}")


def measure_once(binary, prepare, seconds):
    """새 서버를 띄워 상태를 만들고, seconds 동안의 초당 문맥 전환 수를 돌려줍니다."""
    port = free_port()
    server = subprocess.Popen([binary, str(port)], stderr=subprocess.DEVNULL)
    try:
        wait_until_listening(server, port)
        held = prepare(port)
        time.sleep(SETTLE_SECONDS)
        before = context_switches(server.pid)
        time.sleep(seconds)
        after = context_switches(server.pid)
        if held is not None:
            held.close()
        return (after - before) / seconds
    finally:
        server.terminate()
        server.wait()


def prepare_empty(port):
    return None


def prepare_ttl_key(port):
    connection = http.client.HTTPConnection(HOST, port, timeout=5)
    connection.request("PUT", "/keys/idle-wakeup", body=b"x",
                       headers={"TTL": str(TTL_SECONDS)})
    status = connection.getresponse().status
    connection.close()
    if status != 204:
        sys.exit(f"TTL key 저장이 {status}를 받았습니다")
    return None


def prepare_idle_connection(port):
    return socket.create_connection((HOST, port), timeout=5)


def context_switches(pid):
    total = 0
    with open(f"/proc/{pid}/status", encoding="ascii") as status:
        for line in status:
            if line.startswith(("voluntary_ctxt_switches:",
                                "nonvoluntary_ctxt_switches:")):
                total += int(line.split()[1])
    return total


def wait_until_listening(server, port):
    deadline = time.monotonic() + START_TIMEOUT
    while time.monotonic() < deadline:
        if server.poll() is not None:
            sys.exit(f"서버가 시작 중에 끝났습니다: 종료 코드 {server.returncode}")
        try:
            socket.create_connection((HOST, port), timeout=0.2).close()
            return
        except OSError:
            time.sleep(0.02)
    sys.exit(f"서버가 {START_TIMEOUT}초 안에 연결을 받지 않습니다")


def free_port():
    with socket.socket() as probe:
        probe.bind((HOST, 0))
        return probe.getsockname()[1]


if __name__ == "__main__":
    main()
