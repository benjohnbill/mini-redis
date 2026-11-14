#!/usr/bin/env python3
"""캐시 장애 뒤 원본 응답과 안전한 재개를 실제 mini-redis로 확인합니다."""
import os
import sqlite3
import sys
import tempfile
import time

from api_test import Server, expect
from cache_test import Demo, command_check, read_json, stopped_check
from mutation_cache_test import ScriptedCache


def server_on(binary, port):
    for _ in range(50):
        server = Server(binary, port)
        try:
            server.wait_until_listening()
            return server
        except AssertionError:
            server.log.close()
            time.sleep(0.1)
    raise AssertionError(f'포트 {port}에서 서버를 다시 띄우지 못했습니다')


def stats(demo):
    return read_json(demo, '/stats')[1]


def detail(demo, product_id):
    return read_json(demo, f'/products/{product_id}')


def restart_resumes(binary, redis_binary, database):
    redis = Server(redis_binary)
    redis.wait_until_listening()
    demo = Demo(binary, database, ['--cache', 'on', '--cache-url',
                                  f'http://127.0.0.1:{redis.port}'])
    restarted = None
    try:
        demo.wait_until_listening()
        first = detail(demo, 1)
        expect(first[0], 200, '복구 전 상세')
        expect(redis.stop(), 0, '실제 캐시 서버 중단')
        expect(detail(demo, 1), first, '중단 중 원본 응답')
        failed = stats(demo)
        expect((failed['cache_state'], failed['cache_get_errors'],
                failed['cache_recoveries']), ('suspended', 1, 0), '오류 뒤 중단')
        restarted = server_on(redis_binary, redis.port)
        expect(detail(demo, 1), first, '복구 뒤 첫 응답')
        recovered = stats(demo)
        expect((recovered['cache_state'], recovered['cache_probes'],
                recovered['cache_recoveries']), ('active', 1, 1), '상태 확인과 재개')
        expect(recovered['origin_queries'] - failed['origin_queries'], 1,
               '이전 key를 읽지 않고 원본 조회')
        expect(detail(demo, 1), first, '복구 뒤 재사용')
        reused = stats(demo)
        expect(reused['origin_queries'], recovered['origin_queries'], '새 key 적중')
        expect(reused['cache_hits'] - recovered['cache_hits'], 1, '재개 뒤 적중')
    finally:
        stopped_check(demo)
        if restarted is not None:
            stopped_check(restarted)
        if redis.process.poll() is None:
            stopped_check(redis)


def probe_rate_is_limited(binary, redis_binary, database):
    redis = Server(redis_binary)
    redis.wait_until_listening()
    port = redis.port
    expect(redis.stop(), 0, '상태 확인용 서버 중단')
    demo = Demo(binary, database, ['--cache', 'on', '--cache-url',
                                  f'http://127.0.0.1:{port}'])
    restarted = None
    try:
        demo.wait_until_listening()
        expect(detail(demo, 1)[0], 200, '연결 실패 때 원본 응답')
        expect(detail(demo, 1)[0], 200, '한 번 더 원본 응답')
        first = stats(demo)
        expect((first['cache_probes'], first['cache_probe_failures'],
                first['cache_state']), (1, 1, 'suspended'), '1초에 한 번만 확인')
        time.sleep(1.1)
        expect(detail(demo, 1)[0], 200, '다시 확인할 때도 원본 응답')
        second = stats(demo)
        expect(second['cache_probes'], first['cache_probes'] + 1, '1초 뒤 다시 확인')
        restarted = server_on(redis_binary, port)
        time.sleep(1.1)
        expect(detail(demo, 1)[0], 200, '복구 뒤 응답')
        third = stats(demo)
        expect((third['cache_state'], third['cache_recoveries']), ('active', 1), '재개')
        expect(third['origin_queries'] - second['origin_queries'], 1, '재개 뒤 원본 조회')
        expect(detail(demo, 1)[0], 200, '재개 뒤 적중')
        expect(stats(demo)['origin_queries'], third['origin_queries'], '새 namespace 적중')
    finally:
        stopped_check(demo)
        if restarted is not None:
            stopped_check(restarted)


def put_failure_uses_origin(binary, _redis_binary, database):
    peer = ScriptedCache({'GET': (404, b'{"error":"key_not_found"}'),
                          'PUT': (500, b''), 'DELETE': (204, b'')})
    demo = Demo(binary, database, ['--cache', 'on', '--cache-url',
                                  f'http://127.0.0.1:{peer.port}'])
    try:
        demo.wait_until_listening()
        expect(detail(demo, 1), (200, {'id': 1, 'name': '상품 1', 'price': 48271}),
               'PUT 실패에도 원본 응답')
        counts = stats(demo)
        expect((counts['cache_state'], counts['cache_misses'],
                counts['cache_put_failures'], counts['cache_errors']),
               ('suspended', 1, 1, 1), 'PUT 실패는 중단')
        expect(detail(demo, 1)[0], 200, '중단 뒤에도 원본 응답')
        after = stats(demo)
        expect((after['cache_state'], after['cache_puts'],
                after['cache_put_failures'], after['cache_recoveries']),
               ('suspended', 2, 2, 1), '상태 확인 뒤 다시 저장 실패로 중단')
    finally:
        stopped_check(demo)
        peer.stop()


def origin_lock_keeps_hits(binary, redis_binary, database):
    redis = Server(redis_binary)
    redis.wait_until_listening()
    demo = Demo(binary, database, ['--cache', 'on', '--cache-url',
                                  f'http://127.0.0.1:{redis.port}'])
    try:
        demo.wait_until_listening()
        cached = detail(demo, 1)
        database_connection = sqlite3.connect(database)
        try:
            database_connection.execute('BEGIN EXCLUSIVE')
            expect(detail(demo, 1), cached, '잠금 중 적중 유지')
            expect(detail(demo, 2), (503, {'error': 'origin_unavailable'}),
                   '잠금 중 미적중은 503')
        finally:
            database_connection.rollback()
            database_connection.close()
        expect(detail(demo, 2)[0], 200, '잠금 해제 뒤 성공')
        counts = stats(demo)
        expect(counts['cache_state'], 'active', '잠금은 캐시 상태를 바꾸지 않음')
    finally:
        stopped_check(demo)
        stopped_check(redis)


def main():
    binary, redis_binary = map(os.path.abspath, sys.argv[1:])
    with tempfile.TemporaryDirectory() as directory:
        database = os.path.join(directory, 'products.sqlite')
        command_check(binary, ['--db', database, '--seed', '1', '--count', '6'], 0)
        for name, test in [
                ('실제 캐시 재시작 뒤 새 식별자로 재개하고 이전 응답을 쓰지 않는다',
                 restart_resumes),
                ('중단 중 상태 확인은 1초에 한 번만 시도한다', probe_rate_is_limited),
                ('캐시 저장 실패는 원본 응답을 막지 않고 캐시를 중단한다',
                 put_failure_uses_origin),
                ('원본 잠금 중 적중 응답은 유지되고 미적중은 503이다',
                 origin_lock_keeps_hits)]:
            test(binary, redis_binary, database)
            print('PASS:', name)
    print('cache recovery functional: 4개 통과')


if __name__ == '__main__':
    main()
