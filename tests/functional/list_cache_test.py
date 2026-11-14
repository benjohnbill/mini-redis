#!/usr/bin/env python3
"""상품 목록·검색의 조건 처리와 응답 캐시를 실제 mini-redis로 확인합니다."""
import os
import sqlite3
import sys
import tempfile
import time

from api_test import Server, expect
from cache_test import Demo, command_check, read_json, stopped_check

FIXTURE = [(1, 'Apple', 100), (2, 'apple', 100), (3, '상품 한글', 200),
           (4, 'Banana', 50), (5, 'Cherry', 100), (6, 'Date', 50)]
DEFAULTS = '/products'
FULL_DEFAULTS = ('/products?q=&min_price=0&max_price=1000000000&sort=id'
                 '&offset=0&limit=20')


def ids(status_body):
    status, body = status_body
    expect(status, 200, '목록 상태')
    return [item['id'] for item in body['items']]


def defaults_and_reuse(demo):
    _, before = read_json(demo, '/stats')
    first = read_json(demo, DEFAULTS)
    expect(len(first[1]['items']), 20, '기본 limit 20')
    expect([item['id'] for item in first[1]['items']], list(range(1, 21)), '기본 id 정렬')
    expect(first[1]['items'][0],
           {'id': 1, 'name': '상품 1', 'price': 48271}, '기본 첫 상품')
    _, after = read_json(demo, '/stats')
    expect(after['origin_queries'] - before['origin_queries'], 1, '목록 원본 한 번')
    expect(after['cache_misses'] - before['cache_misses'], 1, '첫 목록 미적중')
    expect(after['cache_puts'] - before['cache_puts'], 1, '목록 body 저장')
    expect(read_json(demo, DEFAULTS), first, '같은 조건 재사용')
    _, reused = read_json(demo, '/stats')
    expect(reused['origin_queries'], after['origin_queries'], '적중은 원본 없음')
    expect(reused['cache_hits'] - after['cache_hits'], 1, '같은 조건 적중')


def equivalent_conditions_share_key(demo):
    _, before = read_json(demo, '/stats')
    explicit = read_json(demo, FULL_DEFAULTS)
    expect(explicit, read_json(demo, '/products'), '기본값 명시는 같은 응답')
    expect(read_json(demo, '/products?limit=5&sort=id'),
           read_json(demo, '/products?sort=id&limit=5'), '매개변수 순서 무관')
    expect(read_json(demo, '/products?'), read_json(demo, DEFAULTS), '빈 질의')
    _, after = read_json(demo, '/stats')
    expect(after['origin_queries'] - before['origin_queries'], 1, '동등 조건은 원본 한 번')
    expect(after['cache_misses'] - before['cache_misses'], 1, '동등 조건은 미적중 한 번')
    expect(after['cache_hits'] - before['cache_hits'], 5, '동등 조건은 적중')


def different_conditions_are_separate(demo):
    _, before = read_json(demo, '/stats')
    bodies = [read_json(demo, '/products?limit=6'),
              read_json(demo, '/products?limit=5&min_price=1'),
              read_json(demo, '/products?limit=5&max_price=500000000'),
              read_json(demo, '/products?limit=5&sort=price_asc'),
              read_json(demo, '/products?limit=5&sort=price_desc'),
              read_json(demo, '/products?limit=5&offset=5')]
    for body in bodies:
        expect(len(body[1]['items']) <= 6, True, '조건별 limit')
    expect(len({body[1]['items'][0]['id'] for body in bodies if body[1]['items']}) > 1,
           True, '조건별 응답 구분')
    _, after = read_json(demo, '/stats')
    expect(after['origin_queries'] - before['origin_queries'], 6, '조건마다 원본 조회')
    expect(after['cache_misses'] - before['cache_misses'], 6, '조건마다 미적중')


def search_and_range(fixture):
    expect(ids(read_json(fixture, '/products?q=app')), [2], '부분 문자열과 대소문자')
    expect(ids(read_json(fixture, '/products?q=Apple')), [1], '대문자 구분')
    expect(ids(read_json(fixture, '/products?q=%EC%83%81%ED%92%88')), [3], 'UTF-8 검색')
    expect(ids(read_json(fixture, '/products?q=%25')), [], '와일드카드가 아님')
    expect(ids(read_json(fixture, '/products?min_price=100&max_price=100')),
           [1, 2, 5], '가격 양 끝 포함')
    expect(ids(read_json(fixture, '/products?q=Date&min_price=51')), [],
           '검색과 가격 범위 함께')


def sort_and_pages(fixture):
    expect(ids(read_json(fixture, '/products?sort=price_asc')),
           [4, 6, 1, 2, 5, 3], '가격 오름차순과 동률 id')
    expect(ids(read_json(fixture, '/products?sort=price_desc')),
           [3, 1, 2, 5, 4, 6], '가격 내림차순과 동률 id')
    expect(ids(read_json(fixture, '/products?offset=1&limit=2')), [2, 3], '페이지')
    expect(ids(read_json(fixture, '/products?offset=5&limit=2')), [6], '마지막 페이지')


def empty_list_is_cached(fixture):
    _, before = read_json(fixture, '/stats')
    expect(read_json(fixture, '/products?q=zzz'), (200, {'items': []}), '빈 목록')
    expect(read_json(fixture, '/products?q=zzz'), (200, {'items': []}), '빈 목록 재사용')
    _, after = read_json(fixture, '/stats')
    expect(after['origin_queries'] - before['origin_queries'], 1, '빈 목록도 캐시')
    expect(after['cache_hits'] - before['cache_hits'], 1, '빈 목록 적중')


def invalid_requests_are_rejected(fixture):
    _, before = read_json(fixture, '/stats')
    for path in ['/products?limit=0', '/products?limit=101', '/products?limit=1.5',
                 '/products?limit=abc', '/products?offset=-1',
                 '/products?offset=1000001', '/products?min_price=1000000001',
                 '/products?max_price=-1', '/products?min_price=100&max_price=99',
                 '/products?sort=name', '/products?q=a&q=b', '/products?x=1',
                 '/products?q=%GG', '/products?q=%00', '/products?q=%ED%A0%80',
                 '/products?limit=', '/products?q', '/products?min_price=',
                 f"/products?q={'x' * 121}"]:
        expect(read_json(fixture, path), (400, {'error': 'invalid_request'}), path)
    _, after = read_json(fixture, '/stats')
    expect((after['origin_queries'], after['product_gets'], after['cache_gets']),
           (before['origin_queries'], before['product_gets'], before['cache_gets']),
           '잘못된 요청은 원본과 캐시에 접근하지 않음')


def cache_off_matches_cache_on(binary, database, cached):
    off = Demo(binary, database)
    try:
        off.wait_until_listening()
        for path in ['/products', FULL_DEFAULTS, '/products?sort=price_asc',
                     '/products?q=app', '/products?min_price=100&max_price=100',
                     '/products?offset=1&limit=2', '/products?q=zzz']:
            expect(read_json(off, path), read_json(cached, path), 'on/off 동일 응답')
        _, stats = read_json(off, '/stats')
        expect((stats['cache_gets'], stats['cache_puts'], stats['cache_bypasses']),
               (0, 0, 7), 'off는 캐시 접근을 생략')
    finally:
        stopped_check(off)


def expiry_rereads_origin(demo):
    first = read_json(demo, '/products?limit=3')
    _, before = read_json(demo, '/stats')
    expect(read_json(demo, '/products?limit=3'), first, '만료 전 적중')
    time.sleep(2.5)
    expect(read_json(demo, '/products?limit=3'), first, '만료 뒤 같은 응답')
    _, after = read_json(demo, '/stats')
    expect(after['origin_queries'] - before['origin_queries'], 1, '만료 뒤 원본 재조회')
    expect(after['cache_puts'] - before['cache_puts'], 1, '만료 뒤 다시 저장')


def main():
    binary, redis_binary = map(os.path.abspath, sys.argv[1:])
    with tempfile.TemporaryDirectory() as directory:
        database = os.path.join(directory, 'products.sqlite')
        fixture = os.path.join(directory, 'fixture.sqlite')
        command_check(binary, ['--db', database, '--seed', '1'], 0)
        command_check(binary, ['--db', fixture, '--seed', '1', '--count', '6'], 0)
        with sqlite3.connect(fixture) as database_connection:
            database_connection.execute('PRAGMA ignore_check_constraints=ON')
            database_connection.executemany(
                'UPDATE products SET name=?, price=? WHERE id=?',
                [(name, price, product_id) for product_id, name, price in FIXTURE])
        redis = Server(redis_binary)
        demos = []
        try:
            redis.wait_until_listening()
            arguments = ['--cache', 'on', '--cache-url',
                         f'http://127.0.0.1:{redis.port}']
            demo = Demo(binary, database, arguments)
            demos.append(demo)
            demo.wait_until_listening()
            defaults_and_reuse(demo)
            print("[T19] 기본 조건의 목록을 캐시하고 같은 조건은 원본을 다시 읽지 않는다")
            equivalent_conditions_share_key(demo)
            print('PASS: 매개변수 순서와 기본값 표기가 달라도 같은 캐시를 쓴다')
            different_conditions_are_separate(demo)
            print('PASS: 조건이 다른 목록은 서로 다른 응답으로 분리한다')
            cached = Demo(binary, fixture, arguments)
            demos.append(cached)
            cached.wait_until_listening()
            search_and_range(cached)
            print('PASS: 검색은 대소문자를 구분하고 가격 양 끝을 포함한다')
            sort_and_pages(cached)
            print('PASS: 가격 정렬은 동률을 id로 고정하고 페이지를 나눈다')
            empty_list_is_cached(cached)
            print('PASS: 빈 목록도 정상 응답으로 저장하고 재사용한다')
            invalid_requests_are_rejected(cached)
            print('PASS: 잘못된 목록 매개변수는 원본과 캐시에 접근하기 전에 거절한다')
            cache_off_matches_cache_on(binary, fixture, cached)
            print('PASS: 캐시 off는 같은 응답을 내고 캐시에 접근하지 않는다')
            expiring = Demo(binary, fixture, [*arguments, '--ttl-seconds', '2'])
            demos.append(expiring)
            expiring.wait_until_listening()
            expiry_rereads_origin(expiring)
            print('PASS: 만료 뒤 목록 조회는 원본을 다시 읽는다')
        finally:
            for demo in demos:
                stopped_check(demo)
            stopped_check(redis)
    print('list cache functional: 9개 통과')


if __name__ == '__main__':
    main()
