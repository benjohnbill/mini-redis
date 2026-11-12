#!/usr/bin/env python3
"""인용된 테스트 ID와 ADR 이름이 실제로 있는지 확인합니다.

규칙(CONVENTION.md의 "테스트"):
- 테스트 ID는 테스트 이름 문자열의 맨 앞에 "[T1] ..."처럼 적습니다. ID는 레포 전체에서 하나뿐입니다.
- ID를 인용하는 곳은 두 곳뿐입니다: docs/adr/*.md 의 `결정:` 줄과
  docs/requirements.md 표의 `근거` 칸입니다.
- `근거` 칸에서 백틱으로 감싼 이름 중 테스트 ID가 아닌 것은 ADR 이름입니다.
  docs/adr/<이름>.md 파일이 있어야 합니다.
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# 한국어 조사가 바로 붙어도(T1과) ID로 읽도록, \b 대신 영문자와 숫자만 경계로 봅니다.
TEST_ID = re.compile(r"(?<![A-Za-z0-9_])T[0-9]+(?![A-Za-z0-9_])")
DEFINED_ID = re.compile(r'"\[(T[0-9]+)\]')
BACKTICK = re.compile(r"`([^`]+)`")


def defined_test_ids(errors):
    ids = {}
    for path in sorted((ROOT / "tests").rglob("*")):
        if not path.is_file() or path.name == "acutest.h":
            continue
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            for test_id in DEFINED_ID.findall(line):
                where = f"{path.relative_to(ROOT)}:{lineno}"
                if test_id in ids:
                    errors.append(f"{where}: 테스트 ID가 중복됩니다: {test_id} (먼저 나온 곳: {ids[test_id]})")
                else:
                    ids[test_id] = where
    return ids


def adr_citations():
    """(위치, 테스트 ID) 목록을 돌려줍니다."""
    cites = []
    for path in sorted((ROOT / "docs" / "adr").glob("*.md")):
        for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if "결정:" in line:
                where = f"{path.relative_to(ROOT)}:{lineno}"
                cites += [(where, t) for t in TEST_ID.findall(line)]
    return cites


def requirement_citations(errors):
    """(위치, 테스트 ID) 목록과 (위치, ADR 이름) 목록을 돌려줍니다."""
    path = ROOT / "docs" / "requirements.md"
    test_cites, adr_cites = [], []
    column = None
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.startswith("|"):
            column = None
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if column is None:
            if "근거" not in cells:
                errors.append(f"{path.relative_to(ROOT)}:{lineno}: 표 머리에 `근거` 칸이 없습니다")
                return test_cites, adr_cites
            column = cells.index("근거")
            continue
        if column >= len(cells) or set(cells[0]) <= set("-: "):
            continue
        where = f"{path.relative_to(ROOT)}:{lineno}"
        cell = cells[column]
        test_cites += [(where, t) for t in TEST_ID.findall(cell)]
        for name in BACKTICK.findall(cell):
            if not TEST_ID.fullmatch(name):
                adr_cites.append((where, name))
    return test_cites, adr_cites


def main():
    errors = []
    ids = defined_test_ids(errors)
    req_tests, req_adrs = requirement_citations(errors)
    for where, test_id in adr_citations() + req_tests:
        if test_id not in ids:
            errors.append(f"{where}: 이 테스트 ID를 가진 테스트가 없습니다: {test_id}")
    for where, name in req_adrs:
        if not (ROOT / "docs" / "adr" / f"{name}.md").is_file():
            errors.append(f"{where}: ADR 파일이 없습니다: docs/adr/{name}.md")
    for e in errors:
        print(e, file=sys.stderr)
    if errors:
        return 1
    print(f"ID 검사 통과: 테스트 ID {len(ids)}개")
    return 0


if __name__ == "__main__":
    sys.exit(main())
