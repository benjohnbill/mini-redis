#!/usr/bin/env python3
"""STAGE_TRACE 데모가 남긴 구간 기록을 읽어 구간마다 p50, p90, p95, p99를 표로 냅니다.

사용법: python3 -B scripts/bench/stage_summary.py 기록파일|폴더 ...
            [--kind detail] [--all] [--batch-get]

기록 한 줄은 데모(demo/product_demo.c의 stage_trace_*)가 요청 하나마다 적습니다.

    stage <묶음 번호> <종류> <대기> <처리> <캐시 GET> <원본 조회> <캐시 PUT>
          <붙잡힘> <실제 붙잡힘> <묶음 요청 수>

시간은 ms이고 지나지 않은 구간과 알 수 없는 값은 "-"입니다. 구간마다 그 구간을
지난 요청만 셉니다. 그래서 원본 조회와 캐시 PUT의 개수는 캐시에서 찾지 못한
요청의 수이고, 실제 붙잡힘의 개수가 적으면 그만큼 응답을 쓴 시각을 놓친 것입니다.
cache_bench.py는 측정 구간의 앞뒤에서 /stats를 부르므로, 기본으로 첫 stats와 둘째
stats 사이의 요청만 셉니다. --all은 준비 구간까지 모두 셉니다. 폴더를 주면 안의
*.log를 모두 읽고, 같은 폴더에서 이름이 회 번호(-1, -2, ...)만 다른 파일을 한
표로 합칩니다.

--batch-get은 묶음마다 캐시 GET 횟수와 GET 시간의 합을 세고, 한 묶음의 GET을
왕복 한 번으로 줄였을 때의 묶음 처리 시간을 어림합니다. 묶음 처리 시간은 묶음
시작부터 마지막 요청의 처리가 끝나기까지(대기 + 처리의 최댓값)입니다. 왕복 한
번의 비용은 같은 파일 묶음에서 잰 캐시 GET 한 번의 p50으로 둡니다.
"""

import argparse
import collections
import os
import re
import statistics
import sys

import open_loop

COLUMNS = ("대기", "처리", "캐시 GET", "원본 조회", "캐시 PUT", "붙잡힘",
           "실제 붙잡힘")
GET_INDEX = COLUMNS.index("캐시 GET")
BATCH_LABEL = "묶음 요청 수"
PERCENTS = (50, 90, 95, 99)
RUN_SUFFIX = re.compile(r"-\d+$")
STAGE_PREFIX = "stage "
DROPPED_PREFIX = "dropped "

Record = collections.namedtuple("Record", "batch kind times batch_size")


class Trace:
    """기록 파일 하나에서 읽은 요청들입니다."""

    def __init__(self, path):
        self.path = path
        self.records = []
        self.dropped = 0
        with open(path, encoding="utf-8") as trace_file:
            for line_number, line in enumerate(trace_file, 1):
                self.line_read(line, line_number)

    def line_read(self, line, line_number):
        if line.startswith(DROPPED_PREFIX):
            self.dropped += int(line[len(DROPPED_PREFIX):])
            return
        if not line.startswith(STAGE_PREFIX):
            return
        fields = line.split()
        if len(fields) != 3 + len(COLUMNS) + 1:
            raise SystemExit(f"{self.path}:{line_number}: 칸 수가 다릅니다: "
                             f"{line.rstrip()}")
        times = [None if field == "-" else float(field)
                 for field in fields[3:3 + len(COLUMNS)]]
        self.records.append(Record((self.path, int(fields[1])), fields[2],
                                   times, int(fields[-1])))

    def measured(self, use_all):
        """첫 stats와 둘째 stats 사이의 요청입니다. stats가 둘보다 적으면 모두입니다."""
        marks = [index for index, record in enumerate(self.records)
                 if record.kind == "stats"]
        if use_all or len(marks) < 2:
            return self.records, len(marks) >= 2 or use_all

        return self.records[marks[0] + 1:marks[1]], True


def paths_expand(paths):
    expanded = []
    for path in paths:
        if os.path.isdir(path):
            expanded += sorted(os.path.join(path, name)
                               for name in os.listdir(path)
                               if name.endswith(".log"))
        else:
            expanded.append(path)
    if not expanded:
        raise SystemExit("읽을 기록 파일이 없습니다")

    return expanded


def group_name(path):
    stem = os.path.splitext(os.path.basename(path))[0]

    return os.path.join(os.path.dirname(path), RUN_SUFFIX.sub("", stem))


def percentiles(values):
    ordered = sorted(values)

    return [open_loop.percentile(ordered, percent) for percent in PERCENTS]


def records_select(traces, kind, use_all):
    """측정 구간에서 kind인 요청과, 모든 파일에 측정 구간 표시가 있었는지입니다."""
    selected = []
    windowed = True
    for trace in traces:
        records, has_window = trace.measured(use_all)
        windowed = windowed and has_window
        selected += [record for record in records if record.kind == kind]

    return selected, windowed


def row_print(label, values, digits):
    cells = ([f"{value:.{digits}f}" for value in percentiles(values)] if values
             else ["-"] * len(PERCENTS))
    print(f"| {label} | {len(values):,} | {' | '.join(cells)} |")


def header_print(first, second):
    print(f"| {first} | {second} | " +
          " | ".join(f"p{percent}" for percent in PERCENTS) + " |")
    print("|---|---:|" + "---:|" * len(PERCENTS))


def table_print(name, traces, kind, use_all):
    records, windowed = records_select(traces, kind, use_all)
    columns = [[record.times[i] for record in records
                if record.times[i] is not None] for i in range(len(COLUMNS))]
    dropped = sum(trace.dropped for trace in traces)
    print(f"\n### {name} ({len(traces)}개 파일, {kind})")
    if not windowed:
        print("주의: stats 표시가 둘보다 적은 파일이 있어 그 파일은 모든 요청을 셌습니다")
    if dropped:
        print(f"주의: 묶음이 커서 기록하지 못한 요청이 {dropped}개 있습니다")
    if not records:
        print(f"{kind} 요청이 없습니다")
        return
    print()
    header_print("구간", "개수")
    for label, values in zip(COLUMNS, columns):
        row_print(f"{label} (ms)", values, 3)
    row_print(BATCH_LABEL, [record.batch_size for record in records], 0)


def batch_get_print(name, traces, kind, use_all):
    records, _ = records_select(traces, kind, use_all)
    batches = collections.defaultdict(list)
    for record in records:
        batches[record.batch].append(record)
    gets = [record.times[GET_INDEX] for record in records
            if record.times[GET_INDEX] is not None]
    if not gets:
        print(f"\n### {name}: 캐시 GET이 없습니다")
        return
    round_trip = open_loop.percentile(sorted(gets), 50)
    rows = collections.defaultdict(list)
    for members in batches.values():
        times = [record.times[GET_INDEX] for record in members
                 if record.times[GET_INDEX] is not None]
        if not times:
            continue
        elapsed = max(record.times[0] + record.times[1] for record in members)
        expected = elapsed - sum(times) + round_trip
        rows["GET 횟수"].append(len(times))
        rows["GET 시간 합 (ms)"].append(sum(times))
        rows["묶음 처리 시간 (ms)"].append(elapsed)
        rows["예상 처리 시간 (ms)"].append(expected)
        rows["줄어드는 시간 (ms)"].append(elapsed - expected)
        rows["줄어드는 비율 (%)"].append(100 * (elapsed - expected) / elapsed)
    print(f"\n### {name}: 묶음 {len(rows['GET 횟수']):,}개, "
          f"왕복 한 번 {round_trip:.3f} ms (캐시 GET p50)")
    print()
    print("| 값 | 평균 | " + " | ".join(f"p{percent}" for percent in PERCENTS)
          + " |")
    print("|---|---:|" + "---:|" * len(PERCENTS))
    for label, values in rows.items():
        digits = 0 if label == "GET 횟수" else (1 if "%" in label else 3)
        cells = [f"{value:.{digits}f}" for value in percentiles(values)]
        print(f"| {label} | {statistics.mean(values):.{max(digits, 1)}f} | "
              f"{' | '.join(cells)} |")


def self_check():
    assert group_name("build/detail-zoomed-ttl60-on-3.log") == \
        os.path.join("build", "detail-zoomed-ttl60-on"), "회 번호를 뗀 이름"
    assert percentiles([1.0, 2.0, 3.0, 4.0]) == [2.0, 4.0, 4.0, 4.0], "백분위수"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("paths", nargs="+", help="기록 파일이나 그 폴더")
    parser.add_argument("--kind", default="detail",
                        choices=("detail", "list", "change", "stats", "other"),
                        help="셀 요청 종류. 기본 detail")
    parser.add_argument("--all", action="store_true",
                        help="stats 표시와 상관없이 모든 요청을 셉니다")
    parser.add_argument("--batch-get", action="store_true",
                        help="묶음별 GET 횟수와 합, 왕복 한 번으로 줄인 예상치")
    arguments = parser.parse_args()
    self_check()
    groups = collections.defaultdict(list)
    for path in paths_expand(arguments.paths):
        groups[group_name(path)].append(Trace(path))
    for name, traces in sorted(groups.items()):
        if arguments.batch_get:
            batch_get_print(name, traces, arguments.kind, arguments.all)
        else:
            table_print(name, traces, arguments.kind, arguments.all)


if __name__ == "__main__":
    sys.exit(main())
