#!/usr/bin/env python3
"""선택 PostgreSQL 원본에서 같은 상품 API와 캐시 동작을 확인합니다.

사용법: POSTGRES_URL='postgresql://...' make cache-functional-postgres

기본 빌드와 make verify는 PostgreSQL을 요구하지 않습니다. 이 파일은 실행 중인
PostgreSQL과 libpq로 만든 build/cache-demo-postgres가 있을 때만 실행합니다.
"""

import http.client
import os
import sys

from api_test import Server, expect
from cache_test import Demo, command_check, read_json, stopped_check


def update(demo, product_id, body):
    connection = http.client.HTTPConnection('127.0.0.1', demo.port, timeout=5)
    try:
        connection.request('PUT', f'/products/{product_id}', body=body.encode(),
                           headers={'Content-Type': 'application/json'})
        return connection.getresponse().status
    finally:
        connection.close()


def stats(demo):
    return read_json(demo, '/stats')[1]


def ok_detail(demo):
    expect(read_json(demo, '/products/1'),
           (200, {'id': 1, 'name': '상품 1', 'price': 48271}), '상세')
    expect(read_json(demo, '/products/1'),
           (200, {'id': 1, 'name': '상품 1', 'price': 48271}), '상세 재사용')
    counts = stats(demo)
    expect((counts['origin'], counts['origin_queries'], counts['cache_hits']),
           ('postgresql', 1, 1), '상세 캐시')


def ok_list(demo):
    _, before = read_json(demo, '/stats')
    expect([item['id'] for item in read_json(demo, '/products?limit=3')[1]['items']],
           [1, 2, 3], '목록')
    expect([item['id'] for item in read_json(demo, '/products?q=2')[1]['items']],
           [2], '검색')
    read_json(demo, '/products?limit=3')
    read_json(demo, '/products?q=2')
    counts = stats(demo)
    expect(counts['cache_hits'] - before['cache_hits'], 2, '목록·검색 재사용')
    expect(counts['origin_queries'] - before['origin_queries'], 2, '조건별 원본 조회')


def ok_mutation(demo):
    _, before = read_json(demo, '/stats')
    expect(update(demo, 1, '{"name":"바뀐 이름","price":7}'), 204, '수정')
    expect(read_json(demo, '/products/1'),
           (200, {'id': 1, 'name': '바뀐 이름', 'price': 7}), '수정 뒤 상세')
    expect([item['id'] for item in read_json(demo, '/products?limit=2')[1]['items']],
           [1, 2], '수정 뒤 목록')
    expect(demo.request('DELETE', '/products/2')[0], 204, '삭제')
    expect(read_json(demo, '/products/2'),
           (404, {'error': 'product_not_found'}), '삭제 뒤 상세')
    expect([item['id'] for item in read_json(demo, '/products')[1]['items']],
           [1, 3, 4, 5, 6], '삭제 뒤 목록')
    counts = stats(demo)
    expect((counts['origin_changes'] - before['origin_changes'],
            counts['invalidations'] - before['invalidations']), (2, 2), '변경 카운터')


def main():
    binary, redis_binary, url = sys.argv[1:4]
    binary, redis_binary = os.path.abspath(binary), os.path.abspath(redis_binary)
    command_check(binary, ['--db', 'postgresql://127.0.0.1:1/none', '--seed', '1'], 1)
    command_check(binary, ['--db', url, '--seed', '1', '--count', '6'], 0)
    redis = Server(redis_binary)
    demo = None
    try:
        redis.wait_until_listening()
        demo = Demo(binary, url, ['--cache', 'on', '--cache-url',
                                 f'http://127.0.0.1:{redis.port}'])
        demo.wait_until_listening()
        for name, test in [('PostgreSQL 원본의 상세를 캐시한다', ok_detail),
                           ('PostgreSQL 목록·검색을 조건별로 캐시한다', ok_list),
                           ('PostgreSQL 수정·삭제가 캐시를 무효화한다', ok_mutation)]:
            test(demo)
            print('PASS:', name)
    finally:
        if demo is not None:
            stopped_check(demo)
        stopped_check(redis)
    print('postgres functional: 3개 통과')


if __name__ == '__main__':
    main()
