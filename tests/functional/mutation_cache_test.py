#!/usr/bin/env python3
"""상품 수정·삭제와 상세·목록·검색 캐시 무효화를 실제 mini-redis로 확인합니다."""
import http.client
import json
import os
import sqlite3
import sys
import tempfile

from api_test import Server, expect
from cache_test import Demo, command_check, read_json, stopped_check


def update_request(demo, product_id, body, content_type='application/json'):
    connection = http.client.HTTPConnection('127.0.0.1', demo.port, timeout=5)
    try:
        headers = {'Content-Type': content_type} if content_type else {}
        connection.request('PUT', f'/products/{product_id}', body=body, headers=headers)
        response = connection.getresponse()
        raw = response.read()
        return response.status, json.loads(raw) if raw else None
    finally:
        connection.close()


class ScriptedCache:
    """같은 HTTP 경계에서 GET·PUT·DELETE 응답을 따로 정하는 최소 대역입니다."""

    def __init__(self, answers):
        import socket
        import threading
        self.answers = answers
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen()
        self.listener.settimeout(0.1)
        self.port = self.listener.getsockname()[1]
        self.stopped = False
        self.requests = []
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        while not self.stopped:
            try:
                connection, _ = self.listener.accept()
            except OSError:
                continue
            with connection:
                self.handle(connection)

    def handle(self, connection):
        connection.settimeout(2)
        data = b''
        try:
            while not self.stopped:
                while b'\r\n\r\n' not in data:
                    part = connection.recv(4096)
                    if not part:
                        return
                    data += part
                head, data = data.split(b'\r\n\r\n', 1)
                head = head.decode()
                method, path = head.split(' ')[:2]
                length = 0
                for line in head.split('\r\n')[1:]:
                    if line.lower().startswith('content-length:'):
                        length = int(line.split(':')[1])
                while len(data) < length:
                    data += connection.recv(length - len(data))
                data = data[length:]
                self.requests.append((method, path))
                status, payload = self.answers[method]
                response = (f'HTTP/1.1 {status} X\r\n'
                            f'Content-Length: {len(payload)}\r\n\r\n')
                connection.sendall(response.encode() + payload)
        except (OSError, ValueError):
            pass

    def stop(self):
        self.stopped = True
        self.thread.join(timeout=5)
        self.listener.close()


def stats(demo):
    return read_json(demo, '/stats')[1]


def detail(demo, product_id):
    return read_json(demo, f'/products/{product_id}')


def update_changes_detail_and_lists(demo):
    original = detail(demo, 1)
    expect(original, (200, {'id': 1, 'name': '상품 1', 'price': 48271}), '수정 전 상세')
    cached_list = read_json(demo, '/products')
    expect(cached_list[1]['items'][0]['name'], '상품 1', '수정 전 목록')
    _, before = read_json(demo, '/stats')
    expect(update_request(demo, 1, '{"name":"바뀐 이름","price":10}'.encode())[:2],
           (204, None), '수정 성공')
    expect(detail(demo, 1), (200, {'id': 1, 'name': '바뀐 이름', 'price': 10}),
           '수정 뒤 상세')
    expect(read_json(demo, '/products')[1]['items'][0]['name'], '바뀐 이름', '수정 뒤 목록')
    after = stats(demo)
    expect(after['origin_changes'] - before['origin_changes'], 1, '원본 변경 수')
    expect(after['invalidations'] - before['invalidations'], 1, '논리적 무효화 수')
    expect(after['cache_deletes'] - before['cache_deletes'], 1, '상세 key 삭제')
    expect(after['cache_delete_failures'], before['cache_delete_failures'], '삭제 성공')
    expect(after['origin_queries'] - before['origin_queries'], 2, '상세와 목록을 다시 읽음')


def search_and_price_edges_follow(demo):
    update_request(demo, 2, '{"name":"상품 2","price":0}'.encode())
    expect(detail(demo, 2), (200, {'id': 2, 'name': '상품 2', 'price': 0}), '가격 0')
    ids = [item['id'] for item in read_json(demo, '/products?min_price=0&max_price=0')[1]['items']]
    expect(ids, [2], '가격 범위 새 진입')
    update_request(demo, 2, '{"name":"상품 2","price":500}'.encode())
    expect(read_json(demo, '/products?min_price=0&max_price=0')[1]['items'], [], '가격 범위 이탈')
    expect([item['id'] for item in read_json(demo, '/products?q=2&max_price=500')[1]['items']],
           [2], '검색 결과 진입과 정렬')
    update_request(demo, 2, b'{"name":"renamed","price":500}')
    expect([item['id'] for item in read_json(demo, '/products?q=2')[1]['items']], [],
           '이름 변경으로 검색 이탈')


def other_detail_cache_survives(demo):
    other = detail(demo, 5)
    expect(other[0], 200, '다른 상품 상세')
    _, before = read_json(demo, '/stats')
    update_request(demo, 3, '{"name":"상품 3","price":914720636}'.encode())
    expect(detail(demo, 5), other, '다른 상품 상세 유지')
    after = stats(demo)
    expect(after['origin_queries'], before['origin_queries'], '다른 상품은 원본을 읽지 않음')
    expect(after['cache_hits'] - before['cache_hits'], 1, '다른 상품 적중 유지')


def delete_removes_product(demo):
    before_delete = read_json(demo, '/products')
    expect(4 in [item['id'] for item in before_delete[1]['items']], True, '삭제 전 목록')
    status, _, raw = demo.request('DELETE', '/products/4')
    expect((status, raw), (204, b''), '삭제 성공')
    expect(detail(demo, 4), (404, {'error': 'product_not_found'}), '삭제 뒤 상세')
    expect([item['id'] for item in read_json(demo, '/products')[1]['items']],
           [item['id'] for item in before_delete[1]['items'] if item['id'] != 4],
           '삭제 뒤 목록')
    _, before = read_json(demo, '/stats')
    expect(demo.request('DELETE', '/products/4')[:1], (404,), '없는 상품 삭제')
    after = stats(demo)
    expect((after['origin_changes'], after['invalidations']),
           (before['origin_changes'], before['invalidations']), '실패한 삭제는 무효화하지 않음')


def failed_update_keeps_cache(demo):
    cached_detail = detail(demo, 3)
    cached_list = read_json(demo, '/products?sort=price_desc')
    _, before = read_json(demo, '/stats')
    database = sqlite3.connect(demo.database)
    try:
        database.execute('BEGIN EXCLUSIVE')
        expect(update_request(demo, 3, b'{"name":"locked","price":1}')[:2],
               (503, {'error': 'origin_unavailable'}), '잠긴 원본 수정')
    finally:
        database.rollback()
        database.close()
    expect(detail(demo, 3), cached_detail, '실패한 수정은 기존 상세 유지')
    expect(read_json(demo, '/products?sort=price_desc'), cached_list, '실패한 수정은 목록 유지')
    after = stats(demo)
    expect((after['origin_queries'], after['origin_changes'], after['invalidations']),
           (before['origin_queries'], before['origin_changes'], before['invalidations']),
           '실패한 수정은 캐시를 무효화하지 않음')


def rejected_updates(demo):
    _, before = read_json(demo, '/stats')
    cases = [(b'{"name":"a","price":1,"extra":2}', 400),
             (b'{"name":"a"}', 400), (b'{"price":1}', 400),
             (b'{"name":"a","name":"b","price":1}', 400),
             (b'{"name":"a","price":1,"price":2}', 400),
             (b'{"name":"","price":1}', 400), (b'{"name":"a","price":-1}', 400),
             (b'{"name":"a","price":1000000001}', 400),
             (b'{"name":"a","price":1.5}', 400), (b'{"name":"a","price":"1"}', 400),
             (b'{"name":"a","price":1}extra', 400), (b'[{"name":"a","price":1}]', 400),
             (b'{"name":"\\xff","price":1}', 400),
             (b'{"name":"' + b'x' * 121 + b'","price":1}', 400),
             (b'{"name":"a","price":1}', 415)]
    for body, status in cases:
        content_type = 'text/plain' if status == 415 else 'application/json'
        got, _ = update_request(demo, 1, body, content_type)
        expect(got, status, '잘못된 수정: ' + repr(body[:24]))
    expect(update_request(demo, 1, b'x' * 4097)[:2], (413, {'error': 'body_too_large'}),
           '수정 body 상한')
    expect(update_request(demo, 10001, b'{"name":"a","price":1}')[:2],
           (404, {'error': 'product_not_found'}), '없는 상품 수정')
    expect(demo.request('PUT', '/products/1',
                        body=b'{"name":"a","price":1}')[:3:2],
           (415, b'{"error":"unsupported_media_type"}'), 'Content-Type 없음')
    expect(demo.request('DELETE', '/products/1', body=b'x')[:1], (400,), '삭제 body')
    expect(demo.request('DELETE', '/products/10001')[:1], (404,), '없는 상품 삭제')
    after = stats(demo)
    expect((after['origin_queries'], after['origin_changes'], after['cache_gets']),
           (before['origin_queries'], before['origin_changes'], before['cache_gets']),
           '거절한 요청은 원본과 캐시에 접근하지 않음')


def invalidation_failure_suspends(binary, database):
    peer = ScriptedCache({'GET': (404, b'{"error":"key_not_found"}'),
                          'PUT': (204, b''), 'DELETE': (500, b'')})
    demo = Demo(binary, database, ['--cache', 'on', '--cache-url',
                                  f'http://127.0.0.1:{peer.port}'])
    try:
        demo.wait_until_listening()
        expect(update_request(demo, 6, '{"name":"수정","price":7}'.encode())[:2],
               (204, None), '무효화 실패에도 수정 성공')
        after = stats(demo)
        expect((after['cache_state'], after['cache_delete_failures'],
                after['origin_changes']), ('suspended', 1, 1), '무효화 실패 뒤 중단')
        expect(detail(demo, 6), (200, {'id': 6, 'name': '수정', 'price': 7}),
               '중단 뒤 원본 응답')
    finally:
        stopped_check(demo)
        peer.stop()


def main():
    binary, redis_binary = map(os.path.abspath, sys.argv[1:])
    with tempfile.TemporaryDirectory() as directory:
        database = os.path.join(directory, 'products.sqlite')
        command_check(binary, ['--db', database, '--seed', '1', '--count', '6'], 0)
        redis = Server(redis_binary)
        demo = None
        try:
            redis.wait_until_listening()
            demo = Demo(binary, database, ['--cache', 'on', '--cache-url',
                                          f'http://127.0.0.1:{redis.port}'])
            demo.wait_until_listening()
            for name, test in [
                    ("[T20] 수정이 상세와 목록 캐시를 무효화하고 새 값을 반환한다", update_changes_detail_and_lists),
                    ('검색 진입과 이탈과 가격 범위가 수정 뒤 반영된다', search_and_price_edges_follow),
                    ('한 상품 수정은 다른 상품의 상세 캐시를 유지한다', other_detail_cache_survives),
                    ('삭제는 상세와 목록에서 상품을 없앤다', delete_removes_product),
                    ('실패한 수정은 기존 원본과 캐시를 유지한다', failed_update_keeps_cache),
                    ('수정 요청 오류와 메서드 계약을 지킨다', rejected_updates)]:
                test(demo)
                print('PASS:', name)
            invalidation_failure_suspends(binary, database)
            print('PASS: commit 뒤 무효화가 실패하면 캐시를 중단하고 수정은 성공으로 응답한다')
        finally:
            if demo is not None:
                stopped_check(demo)
            stopped_check(redis)
    print('mutation cache functional: 7개 통과')


if __name__ == '__main__':
    main()
