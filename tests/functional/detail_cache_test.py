#!/usr/bin/env python3
"""실제 mini-redis와 상품 HTTP API 사이의 상세 캐시를 검증합니다."""
import os
import sys
import tempfile
import sqlite3
import time
import socket
import threading
from api_test import Server, expect
from cache_test import Demo, command_check, read_json, stopped_check


def detail_reuses_cache(demo):
    _, before = read_json(demo, '/stats')
    first = read_json(demo, '/products/1')
    expect(read_json(demo, '/products/%31'), first, '같은 상품 캐시')
    _, after = read_json(demo, '/stats')
    expect(after['origin_queries'] - before['origin_queries'], 1, '원본은 한 번만 조회')
    expect(after['cache_hits'] - before['cache_hits'], 1, '두 번째 조회 적중')
    expect(after['cache_misses'] - before['cache_misses'], 1, '첫 조회 미적중')
    expect(after['cache_puts'] - before['cache_puts'], 1, '정상 body 저장')
    expect(after['cache_state'], 'active', '활성 캐시')



def detail_keys_are_separate(demo):
    _, before = read_json(demo, '/stats')
    first = read_json(demo, '/products/3')
    second = read_json(demo, '/products/4')
    expect(first[1]['id'], 3, '첫 상품')
    expect(second[1]['id'], 4, '다른 상품')
    expect(read_json(demo, '/products/3'), first, '첫 상품 재조회')
    expect(read_json(demo, '/products/4'), second, '다른 상품 재조회')
    _, after = read_json(demo, '/stats')
    expect(after['origin_queries'] - before['origin_queries'], 2, '상품별 원본 조회')
    expect(after['cache_hits'] - before['cache_hits'], 2, '상품별 캐시')


def errors_are_not_cached(demo):
    _, before = read_json(demo, '/stats')
    for _ in range(2):
        expect(read_json(demo, '/products/10001'),
               (404, {'error': 'product_not_found'}), '없는 상품')
    read_json(demo, '/products/0')
    _, after = read_json(demo, '/stats')
    expect(after['origin_queries'] - before['origin_queries'], 2, '없는 상품도 원본 재조회')
    expect(after['cache_misses'] - before['cache_misses'], 2, '404 미적중')
    expect(after['cache_puts'], before['cache_puts'], '404와 입력 오류는 저장하지 않음')
    expect(after['cache_gets'] - before['cache_gets'], 2, '잘못된 입력은 캐시 접근 없음')



def origin_errors_are_not_cached(demo):
    _, before = read_json(demo, '/stats')
    database = sqlite3.connect(demo.database)
    try:
        database.execute('BEGIN EXCLUSIVE')
        for _ in range(2):
            expect(read_json(demo, '/products/7'),
                   (503, {'error': 'origin_unavailable'}), '원본 오류')
    finally:
        database.rollback()
        database.close()
    _, after = read_json(demo, '/stats')
    expect(after['cache_puts'], before['cache_puts'], '원본 오류는 저장하지 않음')
    expect(after['origin_failures'] - before['origin_failures'], 2, '원본을 다시 시도')
    expect(read_json(demo, '/products/7')[0], 200, '오류가 캐시에 남지 않음')


def cache_cli_rejects_invalid_options(binary, database):
    base = ['--db', database, '--port', '8081', '--cache', 'on']
    for name, value in [('--ttl-seconds', '0'), ('--ttl-seconds', '-1'),
                        ('--ttl-seconds', '1.5'), ('--cache-timeout-ms', '0'),
                        ('--cache-timeout-ms', '2147483648'),
                        ('--cache-url', 'https://localhost:8080'),
                        ('--cache-url', 'http://localhost:65537'),
                        ('--cache-url', 'http://localhost:0'),
                        ('--cache-url', 'http://localhost:80/x'),
                        ('--cache-url', 'http://localhost:80/?x=1'),
                        ('--cache-url', 'http://user@localhost:80'),
                        ('--cache-url', 'http://localhost:80/\r\nX:x')]:
        command_check(binary, [*base, name, value], 2)
    for name, value in [('--ttl-seconds', '2'), ('--cache-timeout-ms', '100'),
                        ('--cache-url', 'http://localhost:80')]:
        command_check(binary, [*base, name, value, name, value], 2)

def expiry_does_not_slide(demo):
    first = read_json(demo, '/products/8')
    _, before = read_json(demo, '/stats')
    time.sleep(1.2)
    expect(read_json(demo, '/products/8'), first, '만료 전 적중')
    _, within = read_json(demo, '/stats')
    expect(within['cache_hits'] - before['cache_hits'], 1, '보관 기간 안 적중')
    time.sleep(1.2)
    expect(read_json(demo, '/products/8'), first, '만료 뒤 같은 응답')
    _, after = read_json(demo, '/stats')
    expect(after['origin_queries'] - before['origin_queries'], 1, '적중이 보관 기간을 늘리지 않음')
    expect(after['cache_puts'] - before['cache_puts'], 1, '만료 뒤 다시 저장')


def stopped_cache_suspends(demo, redis):
    expect(redis.stop(), 0, '실제 캐시 서버 중단')
    expect(read_json(demo, '/products/15')[0], 200, '중단 중에도 원본 응답')
    _, first = read_json(demo, '/stats')
    expect(first['cache_state'], 'suspended', '오류 뒤 중단 상태')
    expect(first['cache_get_errors'], 1, 'GET 오류')
    expect(first['cache_errors'], 1, '전체 오류')
    expect(first['cache_put_failures'], 0, 'GET 실패 뒤 PUT 생략')
    expect(read_json(demo, '/products/15')[0], 200, '다음 조회도 원본 응답')
    _, second = read_json(demo, '/stats')
    expect(second['cache_gets'], first['cache_gets'], '중단 중 캐시 요청 없음')
    expect(second['cache_bypasses'] - first['cache_bypasses'], 1, '미사용 조회')


def off_skips_cache(binary, database, arguments):
    off = Demo(binary, database, ['--cache', 'off', *arguments[2:]])
    try:
        off.wait_until_listening()
        expect(read_json(off, '/products/1'),
               (200, {'id': 1, 'name': '상품 1', 'price': 48271}), '캐시 off 동일 응답')
        read_json(off, '/products/1')
        _, stats = read_json(off, '/stats')
        expect((stats['cache_gets'], stats['cache_puts'], stats['cache_errors'],
                stats['cache_bypasses'], stats['origin_queries']), (0, 0, 0, 2, 2),
               'off는 중단된 캐시에 연결하지 않음')
    finally:
        stopped_check(off)

class CachePeer:
    """외부 HTTP 오류/무응답과 상대 연결 종료만 관찰합니다."""
    def __init__(self, response):
        self.listener = socket.socket()
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen()
        self.listener.settimeout(5)
        self.port = self.listener.getsockname()[1]
        self.response = response
        self.closed = False
        self.extra = b''
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        try:
            connection, _ = self.listener.accept()
            with connection:
                connection.settimeout(1)
                request = b''
                while b'\r\n\r\n' not in request:
                    part = connection.recv(4096)
                    if not part:
                        return
                    request += part
                if self.response is not None:
                    connection.sendall(self.response)
                while True:
                    part = connection.recv(4096)
                    if not part:
                        self.closed = True
                        return
                    self.extra += part
        except socket.timeout:
            pass

    def stop(self):
        self.thread.join(timeout=6)
        self.listener.close()
        expect(self.thread.is_alive(), False, '캐시 상대 종료')


def failed_cache_uses_origin(binary, database, response, check_close=False):
    peer = CachePeer(response)
    demo = Demo(binary, database, ['--cache', 'on', '--cache-url',
                                  f'http://127.0.0.1:{peer.port}'])
    try:
        demo.wait_until_listening()
        for _ in range(2):
            expect(read_json(demo, '/products/1'),
                   (200, {'id': 1, 'name': '상품 1', 'price': 48271}), '장애 중 원본 응답')
        _, stats = read_json(demo, '/stats')
        expect((stats['cache_get_errors'], stats['cache_misses'],
                stats['cache_puts'], stats['cache_bypasses'], stats['cache_state']),
               (1, 0, 0, 1, 'suspended'), '오류 404/기한 초과는 미적중이 아님')
        if check_close:
            peer.thread.join(timeout=2)
            expect(peer.closed, True, '데모 실행 중 기한 초과 연결을 닫음')
    finally:
        stopped_check(demo)
        peer.stop()
        expect(peer.extra, b'', 'GET 오류 뒤 PUT을 보내지 않음')


def wrong_404_suspends(binary, database):
    for body in [b'{"error":"route_not_found"}', b'<html>not found</html>']:
        response = (b'HTTP/1.1 404 Not Found\r\nContent-Length: ' +
                    str(len(body)).encode() + b'\r\n\r\n' + body)
        failed_cache_uses_origin(binary, database, response)


def main():
    binary, redis_binary = map(os.path.abspath, sys.argv[1:])
    with tempfile.TemporaryDirectory() as directory:
        database = os.path.join(directory, 'products.sqlite')
        command_check(binary, ['--db', database, '--seed', '1'], 0)
        redis = Server(redis_binary)
        demo = None
        try:
            redis.wait_until_listening()
            wrong_404_suspends(binary, database)
            print('PASS: key 미존재가 아닌 404는 캐시 오류로 중단한다')
            failed_cache_uses_origin(binary, database, None, check_close=True)
            print('PASS: 캐시 기한 초과 뒤 원본으로 응답하고 연결을 즉시 닫는다')
            cache_cli_rejects_invalid_options(binary, database)
            print('PASS: 캐시 실행 옵션의 범위와 중복을 검사한다')
            redis.wait_until_listening()
            arguments = ['--cache', 'on', '--cache-url',
                         f'http://127.0.0.1:{redis.port}', '--ttl-seconds', '2']
            demo = Demo(binary, database, arguments)
            demo.wait_until_listening()
            detail_reuses_cache(demo)
            print('PASS: 같은 상품의 정상 응답을 캐시하고 원본을 다시 읽지 않는다')
            previous = demo
            demo = None
            stopped_check(previous)
            demo = Demo(binary, database, arguments)
            demo.wait_until_listening()
            read_json(demo, '/products/1')
            _, restarted = read_json(demo, '/stats')
            expect((restarted['origin_queries'], restarted['cache_misses'],
                    restarted['cache_hits']), (1, 1, 0), '이전 실행 캐시 격리')
            print('PASS: 데모를 다시 실행하면 이전 캐시를 사용하지 않는다')
            for name, test in [('다른 상품의 상세를 구분해 재사용한다', detail_keys_are_separate),
                               ('없는 상품과 잘못된 입력은 저장하지 않는다', errors_are_not_cached),
                               ('원본 오류 응답은 저장하지 않는다', origin_errors_are_not_cached),
                               ('캐시 적중은 보관 기간을 연장하지 않는다', expiry_does_not_slide)]:
                test(demo)
                print('PASS:', name)
            stopped_cache_suspends(demo, redis)
            print('PASS: 실제 캐시 중단 뒤 원본을 사용하고 캐시를 중단한다')
            off_skips_cache(binary, database, arguments)
            print('PASS: 캐시 off는 원본만 읽고 연결을 생략한다')
        finally:
            try:
                if demo is not None:
                    stopped_check(demo)
            finally:
                stopped_check(redis)


if __name__ == '__main__':
    main()
