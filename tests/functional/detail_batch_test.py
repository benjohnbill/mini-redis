#!/usr/bin/env python3
"""상세 조회 자동 묶기(perf)를 실제 mini-redis로 확인합니다.

데모는 한 poll에 받은 상세 조회를 대기 목록에 모았다가, 2개 이상이면 캐시를 POST
/mget 한 번으로 묻습니다. 여러 연결에서 거의 같은 순간에 요청을 보내 묶음이
생기게 하고, /stats의 cache_mgets로 묶음이 생겼는지 확인합니다. 묶음이 생기는지는
타이밍에 달려 있으므로, 생길 때까지 BURST_TRIALS번까지 다시 보냅니다.

캐시에 없던 상품은 묶음의 응답을 먼저 보낸 뒤 저장하고, 저장할 상품이 2개 이상이면
POST /mset 한 번으로 저장합니다. 그 저장이 다음 수정보다 먼저 끝나는지와, mset만
실패할 때의 동작도 확인합니다.
"""
import http.server
import json
import os
import socket
import sys
import tempfile
import threading

from api_test import HOST, REQUEST_TIMEOUT, Server, expect
from cache_test import Demo, command_check, read_json, stopped_check
from list_cache_test import response_read

BURST_CONNECTIONS = 16
BURST_TRIALS = 50
MIXED_IDS = [1, 2, 3, 2, 10000, 10001, 4, 1, 5, 6, 7, 8, 9, 3, 11, 12]
REPEATED_ID = 13
# 다른 확인이 쓰지 않아 캐시에 아직 없는 상품들입니다. 시도마다 하나씩 씁니다.
FRESH_IDS = range(500, 500 + BURST_TRIALS)
ORIGIN_IDS = [21, 22, 23, 21, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35]
UPDATED_ID = 40
# 시도마다 서로 다른 새 상품 BURST_CONNECTIONS개를 쓰는 확인의 첫 상품 번호입니다.
# 다른 확인과 겹치지 않게 떨어뜨려 둡니다.
DISTINCT_BASE = 1000
FAILING_BASE = 2000
# 조회와 수정의 순서를 보는 확인의 첫 상품 번호입니다. 시도마다 두 개씩 씁니다.
ORDER_BASE = 3000


def burst(demo, paths, closed=()):
    """연결마다 요청 하나를 거의 같은 순간에 보내고, 응답을 (상태, body)로
    돌려줍니다. closed에 든 순번의 연결은 보낸 직후 읽지 않고 닫으며, 그 자리는
    None입니다."""
    sockets = [socket.create_connection((HOST, demo.port), timeout=REQUEST_TIMEOUT)
               for _ in paths]
    try:
        for raw, path in zip(sockets, paths):
            raw.sendall(f'GET {path} HTTP/1.1\r\nHost: {HOST}\r\n\r\n'.encode('ascii'))
        for index in closed:
            sockets[index].close()
        responses = []
        for index, raw in enumerate(sockets):
            if index in closed:
                responses.append(None)
                continue
            with raw.makefile('rb') as stream:
                responses.append(response_read(stream))
        return responses
    finally:
        for raw in sockets:
            raw.close()


def bundled_bursts(demo, paths, closed=()):
    """묶음이 한 번 생길 때까지 burst를 되풀이하고, 모든 시도의 응답을 돌려줍니다."""
    trials = []
    for _ in range(BURST_TRIALS):
        _, before = read_json(demo, '/stats')
        trials.append(burst(demo, paths, closed))
        _, after = read_json(demo, '/stats')
        if after['cache_mgets'] > before['cache_mgets']:
            return trials
    raise AssertionError(f'{BURST_TRIALS}번 보내도 묶음이 생기지 않았습니다')


def expected_bodies(off, paths):
    """cache off 데모가 같은 요청에 주는 응답입니다."""
    return burst(off, paths)


def detail_paths(ids):
    return [f'/products/{product_id}' for product_id in ids]


def bundled_details_match_cache_off(cached, off):
    paths = detail_paths(MIXED_IDS)
    expected = expected_bodies(off, paths)
    expect([status for status, _ in expected],
           [404 if product_id > 10000 else 200 for product_id in MIXED_IDS], '기대 상태')
    # 처음은 미적중이 섞이고, 다시 보내면 적중이 섞입니다.
    for attempt in range(2):
        for responses in bundled_bursts(cached, paths):
            expect(responses, expected, f'{attempt + 1}번째 묶음 응답')


def repeated_product_in_one_bundle(cached, off):
    paths = detail_paths([REPEATED_ID] * BURST_CONNECTIONS)
    expected = expected_bodies(off, paths[:1]) * BURST_CONNECTIONS
    _, before = read_json(cached, '/stats')
    for responses in bundled_bursts(cached, paths):
        expect(responses, expected, '같은 상품만 든 묶음의 응답')
    _, after = read_json(cached, '/stats')
    # 상품이 하나뿐이라, 같은 상품의 요청들은 key 하나를 함께 쓰므로 생긴 묶음은
    # 모두 key를 하나만 담습니다.
    expect(after['cache_mget_keys'] - before['cache_mget_keys'],
           after['cache_mgets'] - before['cache_mgets'], '묶음당 key 1개')
    missed_bundle_loads_once(cached, off)


def missed_bundle_loads_once(cached, off):
    """캐시에 없는 상품을 동시에 요청해, POST /mget이 실제로 미스를 낸 시도에서
    원본 조회와 캐시 저장이 한 번씩만 일어나는지 봅니다. 단건 GET이 하나도 없는
    시도만 셉니다. 단건 GET이 먼저 캐시를 채우면 뒤의 mget이 적중해서, 중복 미스
    처리가 깨져 있어도 횟수가 1로 나오기 때문입니다. 단건 GET이 없으면 첫 조회가
    mget이고, 캐시에 없는 상품이라 그 mget은 반드시 미스입니다. 그런 시도가 나올
    때까지 상품을 바꿔 다시 보냅니다(이 확인을 만들 때 잰 비율은 약 3번에 1번)."""
    counters = ('cache_mgets', 'cache_mget_keys', 'cache_gets',
                'cache_get_errors', 'origin_queries', 'cache_puts')
    for product_id in FRESH_IDS:
        paths = detail_paths([product_id] * BURST_CONNECTIONS)
        _, before = read_json(cached, '/stats')
        responses = burst(cached, paths)
        _, after = read_json(cached, '/stats')
        delta = {name: after[name] - before[name] for name in counters}
        single_gets = delta['cache_gets'] - delta['cache_mget_keys']
        if (delta['cache_mgets'] >= 1 and single_gets == 0 and
                delta['cache_get_errors'] == 0):
            expect(responses, expected_bodies(off, paths[:1]) * BURST_CONNECTIONS,
                   '미스 묶음의 응답')
            expect((delta['origin_queries'], delta['cache_puts']), (1, 1),
                   '미스 묶음의 원본 조회와 캐시 저장 횟수')
            return
    raise AssertionError(f'{BURST_TRIALS}번 보내도 mget이 미스를 낸 시도가 없었습니다')


def detail_after_update_in_same_poll_sees_new_value(cached):
    path = f'/products/{UPDATED_ID}'
    _, old = read_json(cached, path)
    expect(read_json(cached, path), (200, old), '수정 전 캐시 적중')
    update = json.dumps({'name': '묶음 뒤 새 이름', 'price': 777},
                        ensure_ascii=False).encode('utf-8')
    requests = (f'PUT {path} HTTP/1.1\r\nHost: {HOST}\r\nContent-Type: application/json\r\n'
                f'Content-Length: {len(update)}\r\n\r\n').encode('ascii') + update
    requests += f'GET {path} HTTP/1.1\r\nHost: {HOST}\r\n\r\n'.encode('ascii')
    # 한 번에 보내면 데모는 같은 poll에서 수정을 처리한 뒤 조회를 대기 목록에 넣습니다.
    with socket.create_connection((HOST, cached.port), timeout=REQUEST_TIMEOUT) as raw:
        raw.sendall(requests)
        with raw.makefile('rb') as stream:
            updated = response_read(stream)
            fetched = response_read(stream)
    expect(updated, (204, b''), '수정 응답')
    expect(fetched[0], 200, '수정 뒤 조회 상태')
    expect(json.loads(fetched[1]), {'id': UPDATED_ID, 'name': '묶음 뒤 새 이름', 'price': 777},
           '수정 뒤 조회는 새 값')


def closed_connections_in_bundle_are_dropped(cached, off):
    paths = detail_paths(MIXED_IDS)
    expected = expected_bodies(off, paths)
    closed = set(range(0, BURST_CONNECTIONS, 2))
    for responses in bundled_bursts(cached, paths, closed):
        for index, response in enumerate(responses):
            if index not in closed:
                expect(response, expected[index], f'{index}번 연결의 응답')
    expect(read_json(cached, '/stats')[0], 200, '끊긴 뒤에도 통계 응답')
    for responses in bundled_bursts(cached, paths):
        expect(responses, expected, '끊긴 뒤 묶음 응답')


def stopped_cache_answers_from_origin(binary, redis_binary, database, off):
    paths = detail_paths(ORIGIN_IDS)
    expected = expected_bodies(off, paths)
    redis = Server(redis_binary)
    redis.wait_until_listening()
    demo = Demo(binary, database, ['--cache', 'on', '--cache-url',
                                   f'http://127.0.0.1:{redis.port}'])
    try:
        demo.wait_until_listening()
        for responses in bundled_bursts(demo, paths):
            expect(responses, expected, '캐시가 살아 있을 때')
        stopped_check(redis)
        # 캐시가 꺼져 있으면 첫 묶음 조회가 실패해 캐시를 중단하고, 그 뒤로는
        # 묶지 않고 원본만 읽습니다. 그래서 묶음이 생기는지는 따지지 않습니다.
        for _ in range(3):
            expect(burst(demo, paths), expected, '캐시가 꺼진 뒤 원본 응답')
        _, stats = read_json(demo, '/stats')
        expect(stats['cache_state'], 'suspended', '캐시 중단')
    finally:
        stopped_check(demo)
        if redis.process.poll() is None:
            stopped_check(redis)


def distinct_ids(base, trial):
    """시도 하나가 쓰는 서로 다른 새 상품 BURST_CONNECTIONS개입니다."""
    first = base + trial * BURST_CONNECTIONS
    return list(range(first, first + BURST_CONNECTIONS))


def distinct_misses_are_stored_with_one_mset(cached, off):
    """캐시에 없는 서로 다른 상품들을 동시에 요청해, mget이 모두 미스를 낸 시도에서
    묶음마다 저장이 PUT 없이 POST /mset 한 번으로 끝나는지 봅니다. 단건 GET이 없는
    시도만 셉니다. 그러면 모든 요청이 mget 묶음에서 미스를 냈고, 상품이 서로 달라
    묶음마다 저장할 상품이 2개 이상입니다. 요청이 두 poll에 나뉘면 mget과 mset이
    두 번씩입니다."""
    counters = ('cache_mgets', 'cache_mget_keys', 'cache_gets', 'cache_get_errors',
                'cache_msets', 'cache_mset_keys', 'cache_puts')
    for trial in range(BURST_TRIALS):
        paths = detail_paths(distinct_ids(DISTINCT_BASE, trial))
        _, before = read_json(cached, '/stats')
        responses = burst(cached, paths)
        _, after = read_json(cached, '/stats')
        delta = {name: after[name] - before[name] for name in counters}
        single_gets = delta['cache_gets'] - delta['cache_mget_keys']
        if (delta['cache_mgets'] >= 1 and single_gets == 0 and
                delta['cache_get_errors'] == 0):
            expect(responses, expected_bodies(off, paths), '미스 묶음의 응답')
            expect((delta['cache_msets'], delta['cache_mset_keys'], delta['cache_puts']),
                   (delta['cache_mgets'], BURST_CONNECTIONS, 0), '묶음마다 POST /mset 한 번으로 저장')
            return
    raise AssertionError(f'{BURST_TRIALS}번 보내도 mget으로만 처리된 시도가 없었습니다')


class FailingMsetHandler(http.server.BaseHTTPRequestHandler):
    """POST /mset과 상태 확인에만 실패하는 가짜 캐시입니다. 진짜 mini-redis로는
    POST /mget은 성공시키고 POST /mset만 실패시킬 수 없어서 둡니다. 상태 확인도
    실패시켜, 중단한 캐시가 재개하지 않고 suspended로 남게 합니다."""
    protocol_version = 'HTTP/1.1'

    def reply(self, status, body=b'', content_type='application/json'):
        self.send_response(status)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body_read(self):
        return self.rfile.read(int(self.headers.get('Content-Length', 0)))

    def do_GET(self):
        self.body_read()
        if self.path.endswith('probe'):
            self.reply(500, b'{"error":"out_of_memory"}')
        else:
            self.reply(404, b'{"error":"key_not_found"}')

    def do_PUT(self):
        self.body_read()
        self.reply(204)

    def do_DELETE(self):
        self.body_read()
        self.reply(404, b'{"error":"key_not_found"}')

    def do_POST(self):
        body = self.body_read()
        if self.path == '/mget':
            # 데모는 key마다 \n으로 끝나는 줄을 보내므로, 줄 수가 key 수입니다.
            self.reply(200, b'-\n' * body.count(b'\n'), 'application/octet-stream')
        else:
            self.reply(500, b'{"error":"out_of_memory"}')

    def log_message(self, *args):
        pass


def failed_mset_still_answers_and_suspends(binary, database, off):
    """mget이 모두 미스를 내고 그 저장의 POST /mset만 실패할 때, 이미 만든 상품
    응답은 그대로 받고 데모는 캐시를 중단하는지 봅니다."""
    cache = http.server.ThreadingHTTPServer((HOST, 0), FailingMsetHandler)
    thread = threading.Thread(target=cache.serve_forever, daemon=True)
    thread.start()
    demo = Demo(binary, database, ['--cache', 'on', '--cache-url',
                                   f'http://{HOST}:{cache.server_address[1]}'])
    try:
        demo.wait_until_listening()
        for trial in range(BURST_TRIALS):
            paths = detail_paths(distinct_ids(FAILING_BASE, trial))
            _, before = read_json(demo, '/stats')
            responses = burst(demo, paths)
            _, after = read_json(demo, '/stats')
            expect(responses, expected_bodies(off, paths), f'{trial + 1}번째 시도의 응답')
            if after['cache_msets'] > before['cache_msets']:
                expect(after['cache_put_failures'] > before['cache_put_failures'], True,
                       'POST /mset 실패 집계')
                expect(after['cache_state'], 'suspended', '캐시 중단')
                return
        raise AssertionError(f'{BURST_TRIALS}번 보내도 POST /mset을 보낸 시도가 없었습니다')
    finally:
        stopped_check(demo)
        cache.shutdown()
        cache.server_close()


def update_after_bundled_miss_is_not_overwritten(cached):
    """캐시에 없는 서로 다른 상품 X, Y를 두 연결에서 조회하고 곧바로 세 번째 연결에서
    X를 수정한 뒤, X를 다시 조회하면 새 값을 받는지 봅니다. 묶음의 저장(POST /mset)이
    수정보다 늦게 끝나면, 수정이 무효화한 X의 key에 옛 값이 다시 들어가 마지막 조회가
    옛 값을 받습니다. 그 순서를 실제로 지난 시도만 셉니다. X의 첫 조회가 옛 값을
    받았으면 조회가 수정보다 먼저 처리됐고, cache_msets가 늘었으면 X와 Y가 한 묶음에서
    미스를 내 POST /mset으로 저장됐습니다. 마지막 조회는 모든 시도에서 새 값이어야
    합니다."""
    for trial in range(BURST_TRIALS):
        target_id = ORDER_BASE + 2 * trial
        target_path = f'/products/{target_id}'
        other_path = f'/products/{target_id + 1}'
        new_value = {'id': target_id, 'name': f'순서 확인 {trial}', 'price': 900 + trial}
        update = json.dumps({'name': new_value['name'], 'price': new_value['price']},
                            ensure_ascii=False).encode('utf-8')
        requests = [f'GET {path} HTTP/1.1\r\nHost: {HOST}\r\n\r\n'.encode('ascii')
                    for path in (target_path, other_path)]
        requests.append((f'PUT {target_path} HTTP/1.1\r\nHost: {HOST}\r\n'
                         f'Content-Type: application/json\r\n'
                         f'Content-Length: {len(update)}\r\n\r\n').encode('ascii') + update)
        _, before = read_json(cached, '/stats')
        sockets = [socket.create_connection((HOST, cached.port), timeout=REQUEST_TIMEOUT)
                   for _ in requests]
        try:
            for raw, request in zip(sockets, requests):
                raw.sendall(request)
            responses = []
            for raw in sockets:
                with raw.makefile('rb') as stream:
                    responses.append(response_read(stream))
        finally:
            for raw in sockets:
                raw.close()
        _, after = read_json(cached, '/stats')
        expect(responses[2], (204, b''), f'{trial + 1}번째 수정 응답')
        expect(read_json(cached, target_path), (200, new_value), f'{trial + 1}번째 수정 뒤 조회')
        first_name = json.loads(responses[0][1])['name'] if responses[0][0] == 200 else None
        if first_name == f'상품 {target_id}' and after['cache_msets'] > before['cache_msets']:
            return
    raise AssertionError(f'{BURST_TRIALS}번 보내도 조회가 수정보다 먼저 처리되고 '
                         'POST /mset으로 저장된 시도가 없었습니다')


def main():
    binary, redis_binary = map(os.path.abspath, sys.argv[1:])
    with tempfile.TemporaryDirectory() as directory:
        database = os.path.join(directory, 'products.sqlite')
        command_check(binary, ['--db', database, '--seed', '1'], 0)
        redis = Server(redis_binary)
        demos = []
        try:
            redis.wait_until_listening()
            off = Demo(binary, database)
            demos.append(off)
            cached = Demo(binary, database, ['--cache', 'on', '--cache-url',
                                             f'http://127.0.0.1:{redis.port}'])
            demos.append(cached)
            off.wait_until_listening()
            cached.wait_until_listening()
            bundled_details_match_cache_off(cached, off)
            print('PASS: 묶은 상세 조회의 응답은 cache off의 응답과 바이트까지 같다')
            repeated_product_in_one_bundle(cached, off)
            print('PASS: 같은 상품이 한 묶음에 여러 번 와도 모두 같은 응답을 받고, '
                  '캐시에 없으면 원본 조회와 저장은 한 번만 한다')
            detail_after_update_in_same_poll_sees_new_value(cached)
            print('PASS: 같은 poll에서 수정 뒤에 온 상세 조회는 새 값을 받는다')
            closed_connections_in_bundle_are_dropped(cached, off)
            print('PASS: 대기 중에 끊긴 연결의 요청은 버리고 다른 요청은 정상으로 답한다')
            stopped_cache_answers_from_origin(binary, redis_binary, database, off)
            print('PASS: mini-redis가 꺼지면 묶은 상세 조회도 원본으로 정상 응답한다')
            distinct_misses_are_stored_with_one_mset(cached, off)
            print('PASS: 한 묶음에서 미스 난 서로 다른 상품들은 POST /mset 한 번으로 저장한다')
            update_after_bundled_miss_is_not_overwritten(cached)
            print('PASS: 묶음의 저장은 뒤에 온 수정보다 먼저 끝나, 수정 뒤 조회는 새 값을 받는다')
            failed_mset_still_answers_and_suspends(binary, database, off)
            print('PASS: POST /mset만 실패해도 만든 응답은 그대로 보내고 캐시를 중단한다')
        finally:
            for demo in demos:
                stopped_check(demo)
            stopped_check(redis)
    print('detail batch functional: 8개 통과')


if __name__ == '__main__':
    main()
