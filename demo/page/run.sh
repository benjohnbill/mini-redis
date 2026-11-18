#!/usr/bin/env bash
# mini-redis, 데모 백엔드 두 개(cache off, on), 페이지 서버를 함께 띄웁니다.
# Ctrl+C로 모두 끕니다. 실행할 때마다 백엔드를 새로 띄우므로 on 쪽은 빈 캐시로
# 시작합니다(백엔드가 시작할 때 새 실행 식별자를 만듭니다).
# make demo-page가 실행 파일을 만든 뒤 부릅니다.
#   run.sh <데모 백엔드> <mini-redis> <DB>
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BACKEND="$1"
SERVER="$2"
DB="$3"
PAGE_PORT="${PAGE_PORT:-8080}"
OFF_PORT=8081
ON_PORT=8082
REDIS_PORT=8090

# 이미 떠 있는 데모가 포트를 잡고 있으면, 새 프로세스는 바인드에 실패하고 페이지는
# 이전 데모를 계속 보여 줍니다. 그 혼동을 막으려고 먼저 확인합니다.
for port in "$PAGE_PORT" "$OFF_PORT" "$ON_PORT" "$REDIS_PORT"; do
    if (exec 3<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
        echo "run.sh: 포트 ${port}번을 이미 씁니다. 앞서 띄운 데모를 먼저 끕니다" >&2
        exit 1
    fi
done

pids=()
cleanup() {
    kill "${pids[@]}" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

"$SERVER" "$REDIS_PORT" & pids+=($!)
"$BACKEND" --db "$DB" --port "$OFF_PORT" --cache off & pids+=($!)
sleep 0.3
"$BACKEND" --db "$DB" --port "$ON_PORT" --cache on \
    --cache-url "http://127.0.0.1:$REDIS_PORT" --ttl-seconds 600 & pids+=($!)
python3 -B "$HERE/proxy.py" --port "$PAGE_PORT" --off "$OFF_PORT" --on "$ON_PORT" & pids+=($!)
wait -n
