-- wrk 미리 채우기: 서로 다른 key <count>개에 100바이트 value를 한 번씩 저장합니다
-- (spec Q9). key는 prefill-0부터 prefill-<count - 1>까지이고, value는 VALUE입니다.
-- 열린 루프 시나리오(티켓 02)도 이 이름과 value로 key를 고릅니다.
--
-- 사용법: wrk -t<threads> -c<connections> -d<최대 초> -s prefill.lua <url> \
--             -- <count> <threads> <connections> <완료 표시 디렉터리> \
--             [<key 접두사> [<TTL 초>]]
--
-- key 접두사를 주면 prefill- 대신 그 접두사를 씁니다. TTL 초를 주면 저장마다
-- TTL 헤더를 붙입니다(M4 expire 측정의 "한꺼번에 만료").
--
-- wrk는 요청 수가 아니라 시간으로 끝납니다. 그래서 스레드마다 맡은 저장의 응답을 모두
-- 받으면 그 스레드를 멈추고 완료 표시 디렉터리에 done-<스레드 번호> 파일을 만듭니다.
-- scripts/bench/bench.py가 파일이 스레드 수만큼 생기면 wrk에 SIGINT를 보내 끝냅니다.
-- 저장이 오류 응답을 받으면 failed-<스레드 번호> 파일을 남기고 그 스레드를 멈춥니다.
--
-- 저장의 응답(204)은 헤더가 하나도 없고, wrk는 헤더가 없는 응답에는 response()를
-- 부르지 않습니다. 그래서 받은 응답의 수는 request()가 불린 횟수로 셉니다. wrk는
-- 연결마다 처음에 한 번, 그 뒤로는 응답을 하나 다 받을 때마다 한 번 request()를
-- 부르기 때문입니다. 받은 응답 = request() 횟수 - 스레드의 연결 수입니다.

VALUE = string.rep("v", 100)

local next_thread_id = 0

-- 주 Lua 상태에서 스레드를 만들 때마다 불립니다. 스레드 번호를 0부터 붙입니다.
function setup(thread)
    thread:set("thread_id", next_thread_id)
    next_thread_id = next_thread_id + 1
end

-- 아래는 스레드마다의 Lua 상태에서 돕니다. 스레드 i는 key i, i + threads,
-- i + 2 × threads, ...를 맡으므로 스레드끼리 key가 겹치지 않습니다.
function init(args)
    key_count = tonumber(args[1])
    local thread_count = tonumber(args[2])
    stride = thread_count
    -- wrk는 연결을 스레드 수로 나눈 몫만큼 스레드마다 엽니다.
    thread_connections = math.floor(tonumber(args[3]) / thread_count)
    marker_dir = args[4]
    key_prefix = args[5] or "prefill-"
    store_ttl = args[6]
    next_index = thread_id
    owned = 0
    for index = thread_id, key_count - 1, stride do
        owned = owned + 1
    end
    request_calls = 0
    lookups_answered = 0
    finished = false
    -- wrk는 시작 전에 첫 스레드의 request()를 한 번 불러 요청 모양만 보고 보내지
    -- 않습니다(wrk.c의 script_verify_request). 그 호출은 key를 넘기지 않고 세지도
    -- 않습니다.
    is_verify_call_pending = thread_id == 0
end

function request()
    local path = "/keys/" .. key_prefix .. next_index
    if is_verify_call_pending then
        is_verify_call_pending = false
        return store_format(path)
    end
    request_calls = request_calls + 1
    stored_count_check()
    if next_index >= key_count then
        -- 맡은 key를 다 보냈고 남은 응답을 기다립니다. 그동안 연결은 첫 key를
        -- 조회합니다. 조회의 응답은 헤더가 있어 response()가 셉니다.
        return wrk.format("GET", "/keys/" .. key_prefix .. thread_id)
    end
    next_index = next_index + stride
    return store_format(path)
end

-- wrk.format은 받은 헤더 표에 Host와 Content-Length를 써 넣으므로 요청마다 새
-- 표를 만듭니다.
function store_format(path)
    local headers = nil
    if store_ttl then
        headers = { ["TTL"] = store_ttl }
    end
    return wrk.format("PUT", path, headers, VALUE)
end

function response(status, headers, body)
    if status == 200 then
        lookups_answered = lookups_answered + 1
    else
        marker_write("failed-", status)
    end
end

-- 받은 응답에서 조회의 응답을 빼면 저장의 응답입니다.
function stored_count_check()
    local answered = request_calls - thread_connections
    if answered - lookups_answered == owned then
        marker_write("done-", owned)
    end
end

function marker_write(prefix, detail)
    if finished then
        return
    end
    finished = true
    local marker = io.open(marker_dir .. "/" .. prefix .. thread_id, "w")
    marker:write(detail, "\n")
    marker:close()
    wrk.thread:stop()
end

-- 오류가 하나라도 있으면 1로 끝나, 미리 채우기를 실패로 알립니다.
function done(summary, latency, requests)
    local errors = summary.errors
    local error_count = errors.connect + errors.read + errors.write +
                        errors.status + errors.timeout
    if error_count > 0 then
        io.write(string.format(
            "오류: 연결 %d, 읽기 %d, 쓰기 %d, 상태 코드 %d, 시간 초과 %d\n",
            errors.connect, errors.read, errors.write, errors.status,
            errors.timeout))
        os.exit(1)
    end
end
