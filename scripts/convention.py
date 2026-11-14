#!/usr/bin/env python3
"""Team convention checks (CONVENTION.md). Called by `make lint` and `make format`.

  lint    report layout violations (one entry per line, carets under each
          spot), naming, length, and brace violations, and bare `~` in
          Markdown; exit 1 if any
  format  rewrite the files in the team style, escape bare `~` in Markdown,
          and print what changed
"""
import difflib
import os
import re
import subprocess
import sys
import unicodedata
from concurrent.futures import ThreadPoolExecutor

ROOT = subprocess.run(['git', 'rev-parse', '--show-toplevel'],
                      capture_output=True, text=True, check=True).stdout.strip()

# Files we did not write (Pintos, acutest). clang-format and the editor read
# this list directly; lint reads it here, so it lives in one place.
with open(os.path.join(ROOT, '.clang-format-ignore'), encoding='utf-8') as f:
    IGNORED = {line.strip() for line in f
               if line.strip() and not line.startswith('#')}
# Headers we wrote: warnings inside them are shown; the rest are not.
HEADER_FILTER = '(src|tests|demo)/'
EXCLUDE_HEADER_FILTER = '|'.join(re.escape(p) for p in sorted(IGNORED)) or '^$'

COLOR = sys.stdout.isatty()
TIDY_FLAGS = []
if sys.platform == 'darwin':
    # pip의 clang-tidy도 Xcode SDK와 빌드의 Darwin 선언을 사용합니다.
    sdk = subprocess.run(['xcrun', '--show-sdk-path'], capture_output=True,
                         text=True, check=True).stdout.strip()
    TIDY_FLAGS = ['--extra-arg=-D_DARWIN_C_SOURCE',
                  '--extra-arg=-isysroot' + sdk]


def pg_include_flag():
    try:
        proc = subprocess.run(['pg_config', '--includedir'],
                              capture_output=True, text=True)
    except OSError:
        return None
    path = proc.stdout.strip()
    return '-I' + path if proc.returncode == 0 and path else None


# 선택 PostgreSQL 원본은 libpq 헤더가 있을 때만 이름 검사를 합니다. 기본 CI는
# PostgreSQL 개발 파일을 요구하지 않기 때문입니다(M5 spec).
PG_INCLUDE = pg_include_flag()
if PG_INCLUDE:
    TIDY_FLAGS.append('--extra-arg=' + PG_INCLUDE)


def paint(code, s):
    return f'\033[{code}m{s}\033[0m' if COLOR else s


def git_files(*patterns):
    out = subprocess.run(['git', 'ls-files', '--', *patterns], cwd=ROOT,
                         capture_output=True, text=True, check=True).stdout
    return [p for p in out.split() if p not in IGNORED]


def format_files():
    return git_files('src/*.c', 'src/*.h', 'tests/*.c', 'tests/*.h',
                     'demo/*.c', 'demo/*.h')


def lint_files():
    files = git_files('src/*.c', 'tests/*.c', 'demo/*.c')

    return files if PG_INCLUDE else [p for p in files
                                     if p != 'demo/postgres_origin.c']


def formatted(path):
    return subprocess.run(['clang-format', path], cwd=ROOT, capture_output=True,
                          text=True, check=True).stdout


def width(s):
    """Terminal width of s; Hangul and other wide characters take two cells."""
    return sum(2 if unicodedata.east_asian_width(c) in 'WF' else 1 for c in s)


# ── lint: layout ──────────────────────────────────────────────

FORMAT_RE = re.compile(r'^(.+?):(\d+):(\d+): \w+: code should be clang-formatted')


def check_layout(files):
    """Return the number of lines that `make format` would change."""
    proc = subprocess.run(['clang-format', '--dry-run', *files], cwd=ROOT,
                          capture_output=True, text=True)
    spots = {}  # (file, line) -> byte columns
    for line in proc.stderr.splitlines():
        m = FORMAT_RE.match(line)
        if m:
            path, ln, col = m.group(1), int(m.group(2)), int(m.group(3))
            spots.setdefault((os.path.relpath(path, ROOT), ln), []).append(col)

    sources = {}
    for (path, ln), cols in sorted(spots.items()):
        if path not in sources:
            with open(os.path.join(ROOT, path), 'rb') as f:
                sources[path] = f.read().split(b'\n')
        raw = sources[path][ln - 1]
        text = raw.decode('utf-8', 'replace').replace('\t', ' ')
        marks = [' '] * (width(text) + 1)
        for col in cols:
            at = width(raw[:col - 1].decode('utf-8', 'replace'))
            if at < len(marks):
                marks[at] = '^'
        print(f'{path}:{ln}: {paint("33", "layout")}: make format이 고칩니다')
        print(f'{ln:6} | {text}')
        print(f'       | {paint("32", "".join(marks).rstrip())}')
    return len(spots)


# ── lint: naming and braces ───────────────────────────────────

NOISE_RE = re.compile(r'^(\[\d+/\d+\] Processing file |\d+ warnings? (and \d+ errors? )?generated\.|'
                      r'Suppressed \d+ warnings|Use -header-filter|'
                      r'\d+ (warnings?|errors?) generated\.)')


def tidy_one(path):
    proc = subprocess.run(
        ['clang-tidy', '--quiet', '--warnings-as-errors=*',
         f'--header-filter={HEADER_FILTER}',
         f'--exclude-header-filter={EXCLUDE_HEADER_FILTER}',
         *TIDY_FLAGS, path],
        cwd=ROOT, capture_output=True, text=True)
    return (proc.stdout + proc.stderr).splitlines()


def check_names(files):
    """Return the number of naming and brace violations."""
    with ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        outputs = list(pool.map(tidy_one, files))
    seen, block = set(), []
    count = 0

    def flush():
        nonlocal count
        # A header can be reported once per .c file that includes it.
        if block and block[0] not in seen:
            seen.add(block[0])
            count += 1
            print('\n'.join(block))
        block.clear()

    for lines in outputs:
        for line in lines:
            if NOISE_RE.match(line):
                continue
            line = line.replace(ROOT + '/', '').replace(',-warnings-as-errors]', ']')
            if re.match(r'^\S+:\d+:\d+: ', line):
                flush()
                kind = 'braces' if 'braces-around-statements' in line else 'naming'
                if 'identifier-length' in line:
                    kind = 'length'
                line = re.sub(r': error: ', ': ' + paint('31', kind) + ': ', line, count=1)
            block.append(line)
        flush()
    return count


# ── lint: Markdown tilde ──────────────────────────────────────

# GitHub draws text between two single `~` as strikethrough, even across lines
# of one paragraph, so a range such as `1.2~1.5 / 2.0~2.4` hides its middle.
# Every `~` outside code and link targets must be written as `\~`.
TILDE_RE = re.compile(r'(?<![\\~])~(?!~)')
SKIP_RE = re.compile(r'`[^`]*`|\]\([^)]*\)')


def tilde_spots(line):
    """Return the indexes of the bare `~` characters in one Markdown line."""
    skipped = [m.span() for m in SKIP_RE.finditer(line)]

    return [m.start() for m in TILDE_RE.finditer(line)
            if not any(start <= m.start() < end for start, end in skipped)]


def markdown_lines(path):
    """Yield (line number, line) for the lines outside fenced code blocks."""
    in_fence = False
    with open(os.path.join(ROOT, path), encoding='utf-8') as f:
        for ln, line in enumerate(f, 1):
            if line.lstrip().startswith('```'):
                in_fence = not in_fence
            elif not in_fence:
                yield ln, line.rstrip('\n')


def check_tildes(files):
    """Return the number of Markdown lines with a bare `~`."""
    count = 0
    for path in files:
        for ln, line in markdown_lines(path):
            spots = tilde_spots(line)
            if not spots:
                continue
            count += 1
            marks = [' '] * (width(line) + 1)
            for at in spots:
                marks[width(line[:at])] = '^'
            print(f'{path}:{ln}: {paint("33", "tilde")}: make format이 \\~로 고칩니다')
            print(f'{ln:6} | {line}')
            print(f'       | {paint("32", "".join(marks).rstrip())}')
    return count


def escape_tildes(path):
    """Rewrite bare `~` as `\\~` in one Markdown file; return True if changed."""
    full = os.path.join(ROOT, path)
    with open(full, encoding='utf-8') as f:
        lines = f.read().split('\n')
    fixes = dict(markdown_lines(path))
    changed = False
    for ln in fixes:
        spots = tilde_spots(lines[ln - 1])
        for at in reversed(spots):
            lines[ln - 1] = lines[ln - 1][:at] + '\\' + lines[ln - 1][at:]
        changed = changed or bool(spots)
    if changed:
        with open(full, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines))

    return changed


def lint():
    layout = check_layout(format_files())
    names = check_names(lint_files())
    tildes = check_tildes(git_files('*.md'))
    if layout or names or tildes:
        print()
        print(f'모양 {layout}줄, 물결표 {tildes}줄: make format 으로 자동 수정 | '
              f'이름·길이·중괄호 {names}건: 직접 수정 (CONVENTION.md)')
        return 1
    print('lint: 위반 없음')
    return 0


# ── format ────────────────────────────────────────────────────

def fmt():
    changed = 0
    for path in format_files():
        full = os.path.join(ROOT, path)
        with open(full, encoding='utf-8', errors='surrogateescape') as f:
            before = f.read()
        after = formatted(path)
        if before == after:
            continue
        changed += 1
        diff = list(difflib.unified_diff(before.splitlines(), after.splitlines(),
                                         path, path, n=0, lineterm=''))
        print(paint('1', path))
        for d in diff[2:]:
            if d.startswith('@@'):
                print(paint('36', d))
            elif d.startswith('-'):
                print(paint('31', d))
            elif d.startswith('+'):
                print(paint('32', d))
        with open(full, 'w', encoding='utf-8', errors='surrogateescape') as f:
            f.write(after)
    for path in git_files('*.md'):
        if escape_tildes(path):
            changed += 1
            print(paint('1', path) + ': ~ → \\~')
    print(f'format: {changed}개 파일 수정' if changed else 'format: 바꿀 것 없음')
    return 0


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'lint':
        sys.exit(lint())
    if cmd == 'format':
        sys.exit(fmt())
    sys.exit(__doc__)
