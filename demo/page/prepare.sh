#!/usr/bin/env bash
# 데모 페이지의 원본을 준비합니다. 상품을 채운 뒤 이름을 바꿉니다.
# make demo-page-prepare가 실행 파일을 만든 뒤 부릅니다.
#   prepare.sh <데모 백엔드> <DB> <상품 수>
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BACKEND="$1"
DB="$2"
COUNT="$3"

if [[ "$DB" == postgresql://* ]]; then
    # 준비 명령은 products를 교체하는데, 이미 채운 100만 개를 지우는 일이
    # 데모 백엔드의 statement_timeout(1초)을 넘습니다. 빈 상태에서 시작합니다.
    psql "$DB" -q -c "DROP TABLE IF EXISTS products"
fi
"$BACKEND" --db "$DB" --seed 1 --count "$COUNT"
python3 -B "$HERE/fill_names.py" --db "$DB"
