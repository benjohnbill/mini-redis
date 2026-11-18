#!/usr/bin/env python3
"""SQLite 상품 데모의 응답 캐시 효과를 잽니다(M5 spec, 티켓 06·07).

사용법: python3 -B scripts/bench/cache_bench.py build/bench/cache-demo \\
            build/bench/mini-redis [--quick] [--self-check] [--scenario 이름 ...]
            [--rate 초당요청] [--connections 연결수] [--output 파일]
            [--arrival burst|even] [--stage-trace 폴더]

실험마다 같은 seed로 상품 10,000개를 새로 만들고, 새 mini-redis와 새 데모를 띄웁니다.
cache off와 on에 같은 요청 계획을 쓰고, 준비 구간과 측정 구간을 나눕니다. off와 on은
회마다 번갈아 실행합니다. 측정은 지연(p50·p95·p99), 성공 처리량, HTTP 오류, timeout,
적중률, 원본 조회 횟수와 감소율을 회마다 출력하고, 회마다 값의 중앙값과 범위를
더합니다. --output JSON에는 측정한 commit과 회마다 값을 숫자로 적습니다. 기본값은
연결 32개, 초당 500요청, 준비 10초, 측정 30초, 3회입니다. 연결 수와 초당 요청 수는
장비에 맞게 --connections와 --rate로 바꾸고, 개선 전후에 같은 값을 씁니다. --output으로
기록용 결과를 남기는 측정은 3회 이상이어야 합니다. --quick은 준비 1초, 측정 3초,
1회로 함수만 확인할 때 씁니다. 시간 수치는
합격 기준이 아니며 make verify와 CI에서 실행하지 않습니다.
--arrival burst(기본)는 연결마다 같은 시각에 시작하는 시간표를 두어, 연결 수만큼의
요청이 한꺼번에 도착합니다. --arrival even은 시간표 하나가 1/rate초마다 요청을 하나씩
내고 연결들에 차례로 맡깁니다. --stage-trace 폴더는 STAGE_TRACE를 켠 데모(make
cache-stage-bench)에 실행마다 <폴더>/<시나리오>-ttl<TTL>-<off|on>-<회>.log를
STAGE_TRACE_FILE로 넘깁니다.
"""

import argparse
import collections
import http.client
import json
import os
import platform
import random
import signal
import socket
import statistics
import subprocess
import sys
import tempfile
import time
import sqlite3

import open_loop

HOST = "127.0.0.1"
COUNT = 10_000
SEED = 1
CONNECTIONS = 32
RATE = 500
# 몰릴 때와 평상시를 비교하려고, 기본값은 기존과 같음
ARRIVALS = ("burst", "even")
WARMUP_SECONDS = 10
MEASURE_SECONDS = 30
RUNS = 3
RECORD_RUNS_MIN = 3  # 통일안의 "조건당 최소 3회"입니다
START_TIMEOUT = 10.0
STOP_TIMEOUT = 30.0
SEED_TIMEOUT = 120.0
HOT_SHARE = 0.2  # 집중 조회의 80%가 고르는 상품 비율입니다
ZOOM_SHARE = 0.8
SORTS = ("id", "price_asc", "price_desc")
SEARCHES = ("상품 1", "상품 2", "상품 9", "상품 99", "상품 100")
REPEATED_LIST = "/products?min_price=0&max_price=500000000&sort=id&limit=20"
REPEATED_SEARCH = "/products?q=%EC%83%81%ED%92%88%201&sort=price_desc&limit=20"
def scenario_grid():
    grid = {}
    spreads = {"detail": ("uniform", "zoomed"), "list": ("repeated", "varied"),
               "search": ("repeated", "varied")}
    ratios = (("", 0), ("-99:1", 0.01), ("-90:10", 0.1))
    for kind, kind_spreads in spreads.items():
        for spread in kind_spreads:
            for label, ratio in ratios:
                grid[f"{kind}-{spread}{label}"] = (kind, spread, ratio)

    return grid


ALL_SCENARIOS = scenario_grid()


class Process:
    """port를 정해 띄우는 서버 하나입니다. 로그는 끝날 때까지 파일에 둡니다."""

    def __init__(self, command, port, env=None):
        self.port = port
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen(command, stdout=self.log,
                                        stderr=self.log, env=env)

    def wait_until_listening(self):
        deadline = time.monotonic() + START_TIMEOUT
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise SystemExit(f"서버가 시작 중에 끝났습니다: {self.output()}")
            try:
                socket.create_connection((HOST, self.port), timeout=0.2).close()
                return
            except OSError:
                time.sleep(0.02)
        raise SystemExit(f"{START_TIMEOUT}초 안에 연결을 받지 않습니다")

    def stop(self):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
            try:
                self.process.wait(timeout=STOP_TIMEOUT)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        code = self.process.returncode
        output = self.output()
        self.log.close()
        if code != 0:
            raise SystemExit(f"서버가 {code}로 끝났습니다: {output}")

    def output(self):
        self.log.seek(0)

        return self.log.read().decode("utf-8", "replace")


def free_port():
    with socket.socket() as probe:
        probe.bind((HOST, 0))

        return probe.getsockname()[1]


def stats_read(port):
    connection = http.client.HTTPConnection(HOST, port, timeout=5)
    try:
        connection.request("GET", "/stats")
        return json.loads(connection.getresponse().read())
    finally:
        connection.close()


def fetch(port, path):
    connection = http.client.HTTPConnection(HOST, port, timeout=10)
    try:
        connection.request("GET", path)
        response = connection.getresponse()
        body = response.read()
        try:
            parsed = json.loads(body)
        except ValueError:
            parsed = body
        return response.status, parsed
    finally:
        connection.close()


def correctness_check(demo_binary, database, port):
    """변경을 멈춘 뒤 캐시 응답을 같은 DB의 cache off 응답과 대조합니다."""
    hot = Plan("detail", "zoomed").hot
    paths = [f"/products/{product_id}"
             for product_id in [1, 2, 3, COUNT, *hot[:4]]]
    paths += [REPEATED_LIST, REPEATED_SEARCH,
              Plan("list", "varied").list_path(),
              Plan("search", "varied").search_path()]
    origin_port = free_port()
    origin = Process([demo_binary, "--db", database, "--port",
                      str(origin_port), "--cache", "off"], origin_port)
    try:
        origin.wait_until_listening()
        for path in paths:
            cached = fetch(port, path)
            direct = fetch(origin_port, path)
            if cached != direct:
                raise SystemExit(f"정확성 실패: {path}: {cached} != {direct}")
    finally:
        origin.stop()

    return len(paths)


def read_request(path, kind):
    return open_loop.Request(kind, "GET", path, expect_json=True)


class Plan:
    """같은 seed면 같은 요청 순서를 내는 결정적 계획입니다."""

    def __init__(self, kind, spread, write_ratio=0, seed=SEED):
        self.kind = kind
        self.spread = spread
        self.write_ratio = write_ratio
        self.random = random.Random(seed)
        hot = sorted(self.random.sample(range(1, COUNT + 1),
                                        int(COUNT * HOT_SHARE)))
        self.hot = hot
        self.cold = sorted(set(range(1, COUNT + 1)) - set(hot))
        self.price_flip = False

    def product_id(self):
        if self.spread == "zoomed" and self.random.random() < ZOOM_SHARE:
            return self.random.choice(self.hot)

        return self.random.choice(self.cold)

    def request(self, index):
        if self.write_ratio and self.random.random() < self.write_ratio:
            return self.write_request()
        if self.kind == "detail":
            return read_request(f"/products/{self.product_id()}", "detail")
        if self.kind == "list":
            return read_request(self.list_path(), "list")

        return read_request(self.search_path(), "search")

    def list_path(self):
        if self.spread == "repeated":
            return REPEATED_LIST
        low = self.random.randint(0, 500_000_000)
        high = self.random.randint(low, 1_000_000_000)

        return (f"/products?min_price={low}&max_price={high}"
                f"&sort={self.random.choice(SORTS)}"
                f"&offset={self.random.randint(0, 1000)}"
                f"&limit={self.random.randint(1, 100)}")

    def search_path(self):
        if self.spread == "repeated":
            return REPEATED_SEARCH
        query = self.random.choice(SEARCHES)
        encoded = "".join(f"%{byte:02X}" for byte in query.encode())
        low = self.random.randint(0, 500_000_000)
        high = self.random.randint(low, 1_000_000_000)

        return (f"/products?q={encoded}&min_price={low}&max_price={high}"
                f"&sort={self.random.choice(SORTS)}&limit=20")

    def write_request(self):
        product_id = self.product_id()
        self.price_flip = not self.price_flip
        price = 0 if self.price_flip else 1_000_000_000
        body = json.dumps({"name": f"상품 {product_id}", "price": price},
                          ensure_ascii=False).encode()

        return open_loop.Request("write", "PUT", f"/products/{product_id}",
                                 body=body, expect_status=204,
                                 headers=(("Content-Type", "application/json"),))


def execute(port, plan, seconds, load):
    connection_count, rate, arrival = load
    run = open_loop.Run(port, seconds * open_loop.NS_PER_SECOND, strict=False)
    connections = run.connections_open(connection_count)
    if arrival == "burst":
        # 연결마다 시간표를 두면 모든 시간표가 같은 시각에 시작하므로, 연결
        # 수만큼의 요청이 한꺼번에 도착합니다.
        interval = open_loop.NS_PER_SECOND * connection_count // rate
        for connection in connections:
            run.stream_add([connection], interval, plan.request)
    else:
        run.stream_add(connections, open_loop.NS_PER_SECOND // rate,
                       plan.request)
    latencies = run.run()

    return run, latencies


def percentile(values, percent):
    return open_loop.percentile(sorted(values), percent) / open_loop.NS_PER_MS


def successes(latencies):
    return sum(len(values) for kind, values in latencies.items()
               if kind != open_loop.CLIENT_LATE)


def stats_delta(before, after, name):
    return after[name] - before[name]


def run_once(demo_binary, redis_binary, mode, scenario, ttl, warmup, measure,
             database, load, trace_path=None):
    kind, spread, ratio = ALL_SCENARIOS[scenario]
    subprocess.run([demo_binary, "--db", database, "--seed", str(SEED),
                    "--count", str(COUNT)], check=True,
                   timeout=SEED_TIMEOUT)
    redis = None
    demo = None
    try:
        cache_arguments = ["--cache", mode, "--ttl-seconds", str(ttl)]
        if mode == "on":
            cache_port = free_port()
            redis = Process([redis_binary, str(cache_port)], cache_port)
            redis.wait_until_listening()
            cache_arguments += ["--cache-url", f"http://{HOST}:{cache_port}"]
        port = free_port()
        # 정확성 대조용 데모에는 넘기지 않아, 기록 파일에는 측정한 데모만 남습니다.
        demo_env = None
        if trace_path is not None:
            demo_env = {**os.environ, "STAGE_TRACE_FILE": trace_path}
        demo = Process([demo_binary, "--db", database, "--port", str(port),
                        *cache_arguments], port, demo_env)
        demo.wait_until_listening()
        execute(port, Plan(kind, spread, ratio, SEED + 1), warmup, load)
        before = stats_read(port)
        run, latencies = execute(port, Plan(kind, spread, ratio), measure,
                                 load)
        after = stats_read(port)
        checked = correctness_check(demo_binary, database, port) if ratio else 0
    finally:
        if demo is not None:
            demo.stop()
        if redis is not None:
            redis.stop()

    return {"latencies": latencies, "errors": run.errors,
            "timeouts": run.timeouts, "before": before, "after": after,
            "checked": checked}


def record_text(record, seconds):
    latencies = record["latencies"]
    parts = []
    for kind in sorted(key for key in latencies
                       if key != open_loop.CLIENT_LATE):
        values = latencies[kind]
        parts.append(f"{kind} {len(values)}개 p50 {percentile(values, 50):.2f} "
                     f"p95 {percentile(values, 95):.2f} "
                     f"p99 {percentile(values, 99):.2f} ms")
    errors = sum(record["errors"].values())
    throughput = successes(latencies) / seconds
    gets = stats_delta(record["before"], record["after"], "cache_gets")
    hits = stats_delta(record["before"], record["after"], "cache_hits")
    rate = f"{100 * hits / gets:.1f}%" if gets else "계산 불가"
    origin = stats_delta(record["before"], record["after"], "origin_queries")

    checked = (f", 정확성 {record['checked']}개 일치"
               if record["checked"] else "")

    return (f"처리량 {throughput:.1f}/s, " + ", ".join(parts) +
            f", 오류 {errors}, timeout {record['timeouts']}, "
            f"적중률 {rate}, 원본 조회 {origin}{checked}")


def record_numbers(record, seconds):
    """회 하나의 결과를 숫자 칸으로 돌려줍니다. --output JSON에 이 모양으로 적습니다."""
    latencies = record["latencies"]
    kinds = {kind: {"count": len(values), "p50_ms": percentile(values, 50),
                    "p95_ms": percentile(values, 95),
                    "p99_ms": percentile(values, 99)}
             for kind, values in sorted(latencies.items())
             if kind != open_loop.CLIENT_LATE and values}
    before, after = record["before"], record["after"]

    def optional_delta(name):
        # mget, mset 전의 데모는 그 칸이 없어 0입니다.
        return after.get(name, 0) - before.get(name, 0)

    # 캐시 저장 왕복의 전체 수는 cache_puts와 cache_msets로 셉니다. 구간 기록의
    # store 줄은 상세 조회 묶음 끝의 저장만 담기 때문입니다.
    return {"throughput": successes(latencies) / seconds, "latency": kinds,
            "errors": dict(record["errors"]), "timeouts": record["timeouts"],
            "cache_gets": stats_delta(before, after, "cache_gets"),
            "cache_hits": stats_delta(before, after, "cache_hits"),
            "origin_queries": stats_delta(before, after, "origin_queries"),
            "cache_mgets": optional_delta("cache_mgets"),
            "cache_mget_keys": optional_delta("cache_mget_keys"),
            "cache_puts": optional_delta("cache_puts"),
            "cache_msets": optional_delta("cache_msets"),
            "cache_mset_keys": optional_delta("cache_mset_keys"),
            "checked": record["checked"]}


def positive_int(text):
    value = int(text)
    if value <= 0:
        raise argparse.ArgumentTypeError("1 이상이어야 합니다")

    return value


def git_state():
    """측정한 코드의 commit과, 커밋하지 않은 추적 파일 변경이 있는지 돌려줍니다."""
    root = os.path.dirname(os.path.abspath(__file__))
    commit = subprocess.run(["git", "rev-parse", "HEAD"], cwd=root,
                            capture_output=True, text=True, check=True)
    status = subprocess.run(["git", "status", "--porcelain",
                             "--untracked-files=no"], cwd=root,
                            capture_output=True, text=True, check=True)

    return commit.stdout.strip(), bool(status.stdout.strip())


def summarize(records, seconds):
    throughput = sorted(successes(record["latencies"]) / seconds
                        for record in records)
    origins = [stats_delta(record["before"], record["after"], "origin_queries")
               for record in records]
    gets = [stats_delta(record["before"], record["after"], "cache_gets")
            for record in records]
    hits = [stats_delta(record["before"], record["after"], "cache_hits")
            for record in records]

    return {"throughput": throughput, "origins": origins, "gets": gets,
            "hits": hits}


def report(scenario, seconds, results):
    print(f"\n== {scenario}")
    summary = {}
    for mode in ("off", "on"):
        records = results[scenario][mode]
        for index, record in enumerate(records):
            print(f"  {mode} #{index + 1}: {record_text(record, seconds)}")
        summary[mode] = summarize(records, seconds)
        for line in median_lines(mode, records, summary[mode]):
            print(line)
    off_origin = statistics.median(summary["off"]["origins"])
    on_origin = statistics.median(summary["on"]["origins"])
    if off_origin:
        print(f"  원본 조회 감소: {100 * (1 - on_origin / off_origin):.1f}%")
    gets = sum(summary["on"]["gets"])
    hits = sum(summary["on"]["hits"])
    print(f"  cache on 적중률: "
          f"{100 * hits / gets:.1f}%" if gets else "  cache on 적중률: 계산 불가")


def median_lines(mode, records, summary):
    """회마다 값의 중앙값과 범위입니다. 한 회만 튄 값이 결론을 끌지 않게 합니다."""
    lines = [f"  {mode} 중앙값: 처리량 "
             f"{range_text(summary['throughput'], '.1f')}/s, "
             f"원본 조회 {range_text(summary['origins'], '.0f')}"]
    kinds = sorted({kind for record in records for kind in record["latencies"]
                    if kind != open_loop.CLIENT_LATE})
    for kind in kinds:
        runs = [record["latencies"][kind] for record in records
                if record["latencies"].get(kind)]
        p50 = [percentile(values, 50) for values in runs]
        p95 = [percentile(values, 95) for values in runs]
        lines.append(f"    {kind} p50 {range_text(p50, '.2f')} ms, "
                     f"p95 {range_text(p95, '.2f')} ms")

    return lines


def range_text(values, spec):
    """중앙값 (최소~최대) 모양의 글자입니다."""
    return (f"{statistics.median(values):{spec}} "
            f"({min(values):{spec}}~{max(values):{spec}})")


def self_check():
    first = Plan("detail", "zoomed")
    second = Plan("detail", "zoomed")
    pairs = [((first.request(i).method, first.request(i).path),
              (second.request(i).method, second.request(i).path))
             for i in range(200)]
    assert all(left == right for left, right in pairs), "계획이 결정적이지 않음"
    plan = Plan("detail", "zoomed")
    hot = sum(1 for _ in range(10_000)
              if int(plan.request(0).path.rsplit("/", 1)[1]) in plan.hot)
    assert 0.75 <= hot / 10_000 <= 0.85, f"집중 비율 {hot / 10_000}"
    mixed = Plan("detail", "zoomed", 0.1)
    writes = sum(1 for _ in range(10_000)
                 if mixed.request(0).method == "PUT")
    assert 0.08 <= writes / 10_000 <= 0.12, f"수정 비율 {writes / 10_000}"
    repeated = Plan("list", "repeated")
    assert len({repeated.request(i).path for i in range(200)}) == 1, "반복 조건"
    varied = Plan("list", "varied")
    assert len({varied.request(i).path for i in range(200)}) > 190, "다양한 조건"
    assert percentile([2_000_000, 4_000_000], 50) == 2.0, "백분위수"
    assert range_text([3, 1, 2], ".0f") == "2 (1~3)", "중앙값과 범위"
    latencies = collections.defaultdict(list)
    latencies["detail"] = [1, 2, 3]
    latencies[open_loop.CLIENT_LATE] = [9]
    assert successes(latencies) == 3, "성공 집계"
    before = {"cache_gets": 1, "cache_hits": 0, "origin_queries": 5}
    after = {"cache_gets": 4, "cache_hits": 3, "origin_queries": 6}
    assert stats_delta(before, after, "cache_hits") == 3, "통계 차이"
    print("self-check: 계획·비율·집계 통과")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("demo")
    parser.add_argument("redis")
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--self-check", action="store_true")
    parser.add_argument("--scenario", action="append")
    parser.add_argument("--ttl", action="append", type=int)
    parser.add_argument("--kind", action="append",
                        help="detail, list, search 중 실험할 종류")
    parser.add_argument("--warmup", type=int)
    parser.add_argument("--measure", type=int)
    parser.add_argument("--runs", type=int)
    parser.add_argument("--rate", type=positive_int, default=RATE,
                        help="초당 요청 수")
    parser.add_argument("--connections", type=positive_int,
                        default=CONNECTIONS, help="연결 수")
    parser.add_argument("--arrival", choices=ARRIVALS, default="burst",
                        help="요청이 도착하는 모양. 기본 burst")
    parser.add_argument("--output")
    parser.add_argument("--db", help="PostgreSQL 연결 문자열. 없으면 임시 SQLite 파일")
    parser.add_argument("--stage-trace", metavar="DIR",
                        help="STAGE_TRACE 데모가 실행마다 구간 기록을 남길 폴더")
    arguments = parser.parse_args()
    self_check()
    if arguments.stage_trace:
        os.makedirs(arguments.stage_trace, exist_ok=True)
    demo_binary = os.path.abspath(arguments.demo)
    redis_binary = os.path.abspath(arguments.redis)
    warmup, measure, runs = (WARMUP_SECONDS, MEASURE_SECONDS, RUNS)
    if arguments.quick:
        warmup, measure, runs = 1, 3, 1
    warmup = arguments.warmup if arguments.warmup is not None else warmup
    measure = arguments.measure if arguments.measure is not None else measure
    runs = arguments.runs if arguments.runs is not None else runs
    if arguments.output and runs < RECORD_RUNS_MIN:
        raise SystemExit(f"--output으로 남기는 측정은 {RECORD_RUNS_MIN}회 이상이어야 "
                         f"합니다(지금 {runs}회)")
    ttls = arguments.ttl or [60, 2]
    scenarios = arguments.scenario or list(ALL_SCENARIOS)
    if arguments.kind:
        scenarios = [name for name in scenarios
                     if name.split("-")[0] in arguments.kind]
    commit, dirty = git_state()
    print(f"build: {demo_binary}, {redis_binary}")
    print(f"commit: {commit}" + (" (커밋하지 않은 변경 있음)" if dirty else ""))
    print(f"machine: {platform.platform()}, python {platform.python_version()}, "
          f"sqlite {sqlite3.sqlite_version}")
    print(f"input: 상품 {COUNT}개 seed {SEED}, 연결 {arguments.connections}, "
          f"초당 {arguments.rate}요청, 도착 {arguments.arrival}, 준비 {warmup}s, "
          f"측정 {measure}s, {runs}회, TTL {ttls}")
    results = {}
    with tempfile.TemporaryDirectory() as directory:
        database = arguments.db or os.path.join(directory, "products.sqlite")
        run_all(arguments, scenarios, ttls, warmup, measure, runs, demo_binary,
                redis_binary, database, results)
    if arguments.output:
        with open(arguments.output, "w", encoding="utf-8") as output:
            json.dump({"commit": commit, "dirty": dirty,
                       "inputs": {"count": COUNT, "seed": SEED,
                                  "connections": arguments.connections,
                                  "rate": arguments.rate,
                                  "arrival": arguments.arrival,
                                  "warmup": warmup, "measure": measure,
                                  "runs": runs, "ttl": ttls,
                                  "scenarios": scenarios,
                                  "database": arguments.db or "sqlite"},
                       "results": {
                           key: {mode: [record_numbers(record, measure)
                                        for record in records]
                                 for mode, records in modes.items()}
                           for key, modes in results.items()}},
                      output, ensure_ascii=False, indent=2)
        print(f"\n결과를 {arguments.output}에 적었습니다")


def run_all(arguments, scenarios, ttls, warmup, measure, runs, demo_binary,
            redis_binary, database, results):
    for scenario in scenarios:
        for ttl in ttls:
            key = f"{scenario} ttl {ttl}s"
            print(f"\n### {key} 시작", flush=True)
            results[key] = {"off": [], "on": []}
            load = (arguments.connections, arguments.rate, arguments.arrival)
            # off를 모두 잰 뒤에 on을 재면 그동안 바뀐 머신 상태가 한쪽에만
            # 들어가므로, 회마다 off와 on을 번갈아 잽니다.
            for run_index in range(runs):
                for mode in ("off", "on"):
                    trace_path = None
                    if arguments.stage_trace:
                        trace_path = os.path.join(
                            arguments.stage_trace,
                            f"{scenario}-ttl{ttl}-{mode}-{run_index + 1}.log")
                    results[key][mode].append(run_once(
                        demo_binary, redis_binary, mode, scenario, ttl,
                        warmup, measure, database, load, trace_path))
            report(key, measure, results)


if __name__ == "__main__":
    main()
