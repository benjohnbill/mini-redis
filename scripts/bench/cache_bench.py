#!/usr/bin/env python3
"""SQLite 상품 데모의 응답 캐시 효과를 잽니다(M5 spec, 티켓 06·07).

사용법: python3 -B scripts/bench/cache_bench.py build/bench/cache-demo \\
            build/bench/mini-redis [--quick] [--self-check] [--scenario 이름 ...]

실험마다 같은 seed로 상품 10,000개를 새로 만들고, 새 mini-redis와 새 데모를 띄웁니다.
cache off와 on에 같은 요청 계획을 쓰고, 준비 구간과 측정 구간을 나눕니다. 측정은
지연(p50·p95·p99), 성공 처리량, HTTP 오류, timeout, 적중률, 원본 조회 횟수와 감소율,
반복 편차를 출력합니다. 기본값은 연결 32개, 초당 500요청, 준비 10초, 측정 30초, 3회
입니다. --quick은 준비 1초, 측정 3초, 1회로 함수만 확인할 때 씁니다. 시간 수치는
합격 기준이 아니며 make verify와 CI에서 실행하지 않습니다.
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
WARMUP_SECONDS = 10
MEASURE_SECONDS = 30
RUNS = 3
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

    def __init__(self, command, port):
        self.port = port
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen(command, stdout=self.log,
                                        stderr=self.log)

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


def execute(port, plan, seconds):
    run = open_loop.Run(port, seconds * open_loop.NS_PER_SECOND, strict=False)
    connections = run.connections_open(CONNECTIONS)
    interval = open_loop.NS_PER_SECOND * CONNECTIONS // RATE
    for connection in connections:
        run.stream_add([connection], interval, plan.request)
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
             database):
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
        demo = Process([demo_binary, "--db", database, "--port", str(port),
                        *cache_arguments], port)
        demo.wait_until_listening()
        execute(port, Plan(kind, spread, ratio, SEED + 1), warmup)
        before = stats_read(port)
        run, latencies = execute(port, Plan(kind, spread, ratio), measure)
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


def report(scenario, runs, seconds, results):
    print(f"\n== {scenario}")
    summary = {}
    for mode in ("off", "on"):
        records = results[scenario][mode]
        for index, record in enumerate(records):
            print(f"  {mode} #{index + 1}: {record_text(record, seconds)}")
        summary[mode] = summarize(records, seconds)
        throughput = summary[mode]["throughput"]
        origin = summary[mode]["origins"]
        print(f"  {mode} 평균: 처리량 {sum(throughput) / len(throughput):.1f}/s"
              f" (최소 {throughput[0]:.1f}, 최대 {throughput[-1]:.1f}),"
              f" 원본 조회 평균 {sum(origin) / len(origin):.1f}")
    off_origin = sum(summary["off"]["origins"]) / runs
    on_origin = sum(summary["on"]["origins"]) / runs
    if off_origin:
        print(f"  원본 조회 감소: {100 * (1 - on_origin / off_origin):.1f}%")
    gets = sum(summary["on"]["gets"])
    hits = sum(summary["on"]["hits"])
    print(f"  cache on 적중률: "
          f"{100 * hits / gets:.1f}%" if gets else "  cache on 적중률: 계산 불가")


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
    parser.add_argument("--output")
    parser.add_argument("--db", help="PostgreSQL 연결 문자열. 없으면 임시 SQLite 파일")
    arguments = parser.parse_args()
    self_check()
    demo_binary = os.path.abspath(arguments.demo)
    redis_binary = os.path.abspath(arguments.redis)
    warmup, measure, runs = (WARMUP_SECONDS, MEASURE_SECONDS, RUNS)
    if arguments.quick:
        warmup, measure, runs = 1, 3, 1
    warmup = arguments.warmup if arguments.warmup is not None else warmup
    measure = arguments.measure if arguments.measure is not None else measure
    runs = arguments.runs if arguments.runs is not None else runs
    ttls = arguments.ttl or [60, 2]
    scenarios = arguments.scenario or list(ALL_SCENARIOS)
    if arguments.kind:
        scenarios = [name for name in scenarios
                     if name.split("-")[0] in arguments.kind]
    print(f"build: {demo_binary}, {redis_binary}")
    print(f"machine: {platform.platform()}, python {platform.python_version()}, "
          f"sqlite {sqlite3.sqlite_version}")
    print(f"input: 상품 {COUNT}개 seed {SEED}, 연결 {CONNECTIONS}, "
          f"초당 {RATE}요청, 준비 {warmup}s, 측정 {measure}s, {runs}회, TTL {ttls}")
    results = {}
    with tempfile.TemporaryDirectory() as directory:
        database = arguments.db or os.path.join(directory, "products.sqlite")
        run_all(arguments, scenarios, ttls, warmup, measure, runs, demo_binary,
                redis_binary, database, results)
    if arguments.output:
        with open(arguments.output, "w", encoding="utf-8") as output:
            json.dump({"inputs": {"count": COUNT, "seed": SEED,
                                  "connections": CONNECTIONS, "rate": RATE,
                                  "warmup": warmup, "measure": measure,
                                  "runs": runs, "ttl": ttls,
                                  "scenarios": scenarios,
                                  "database": arguments.db or "sqlite"},
                       "results": {
                           key: {mode: [record_text(record, measure)
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
            for mode in ("off", "on"):
                for _ in range(runs):
                    results[key][mode].append(run_once(
                        demo_binary, redis_binary, mode, scenario, ttl,
                        warmup, measure, database))
            report(key, runs, measure, results)


if __name__ == "__main__":
    main()
