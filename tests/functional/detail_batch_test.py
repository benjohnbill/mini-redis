#!/usr/bin/env python3
"""상세 조회 자동 묶기(perf)를 실제 mini-redis로 확인합니다.

데모는 한 poll에 받은 상세 조회를 대기 목록에 모았다가, 2개 이상이면 캐시를 POST
/mget 한 번으로 묻습니다. 여러 연결에서 거의 같은 순간에 요청을 보내 묶음이
생기게 하고, /stats의 cache_mgets로 묶음이 생겼는지 확인합니다. 묶음이 생기는지는
타이밍에 달려 있으므로, 생길 때까지 BURST_TRIALS번까지 다시 보냅니다.
"""
import json
import os
import socket
import sys
import tempfile

from api_test import HOST, REQUEST_TIMEOUT, Server, expect
from cache_test import Demo, command_check, read_json, stopped_check
from list_cache_test import response_read

BURST_CONNECTIONS = 16
BURST_TRIALS = 50
MIXED_IDS = [1, 2, 3, 2, 10000, 10001, 4, 1, 5, 6, 7, 8, 9, 3, 11, 12]
REPEATED_ID = 13
ORIGIN_IDS = [21, 22, 23, 21, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35]
UPDATED_ID = 40


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
            print('PASS: 같은 상품이 한 묶음에 여러 번 와도 모두 같은 응답을 받는다')
            detail_after_update_in_same_poll_sees_new_value(cached)
            print('PASS: 같은 poll에서 수정 뒤에 온 상세 조회는 새 값을 받는다')
            closed_connections_in_bundle_are_dropped(cached, off)
            print('PASS: 대기 중에 끊긴 연결의 요청은 버리고 다른 요청은 정상으로 답한다')
            stopped_cache_answers_from_origin(binary, redis_binary, database, off)
            print('PASS: mini-redis가 꺼지면 묶은 상세 조회도 원본으로 정상 응답한다')
        finally:
            for demo in demos:
                stopped_check(demo)
            stopped_check(redis)
    print('detail batch functional: 5개 통과')


if __name__ == '__main__':
    main()
