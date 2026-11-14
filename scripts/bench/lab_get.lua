-- wrk 실험실: 미리 채운 key prefill-0부터 prefill-<count - 1> 가운데 하나를 골라
-- 작은 GET만 보냅니다(spec Q3). 끝나면 p50, p99, 초당 처리량을 한 줄로 출력하고,
-- 오류 응답이나 소켓 오류가 하나라도 있으면 1로 끝나 측정을 실패로 알립니다.
--
-- 사용법: wrk -t<threads> -c100 -d<초> -s lab_get.lua <url> -- <count>

local next_thread_id = 0

function setup(thread)
    thread:set("thread_id", next_thread_id)
    next_thread_id = next_thread_id + 1
end

function init(args)
    key_count = tonumber(args[1])
    math.randomseed(thread_id + 1)
end

function request()
    return wrk.format("GET", "/keys/prefill-" .. math.random(0, key_count - 1))
end

function done(summary, latency, requests)
    local errors = summary.errors
    local error_count = errors.connect + errors.read + errors.write +
                        errors.status + errors.timeout
    io.write(string.format(
        "실험실: p50 %.3f ms, p99 %.3f ms, 초당 %.0f개, 응답 %d개, 오류 %d개\n",
        latency:percentile(50) / 1000, latency:percentile(99) / 1000,
        summary.requests / (summary.duration / 1e6), summary.requests,
        error_count))
    if error_count > 0 then
        io.write(string.format(
            "오류: 연결 %d, 읽기 %d, 쓰기 %d, 상태 코드 %d, 시간 초과 %d\n",
            errors.connect, errors.read, errors.write, errors.status,
            errors.timeout))
        os.exit(1)
    end
end
