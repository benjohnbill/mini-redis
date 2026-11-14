#!/usr/bin/env python3
"""상품 데모의 실제 HTTP 경계와 공개 통계를 확인합니다."""
import http.client
import json
import os
import signal
import sqlite3
import subprocess
import sys
import tempfile
from api_test import Server, free_port, expect, SANITIZER_MARKS


class Demo(Server):
    def __init__(self, binary, database, cache_arguments=None):
        self.binary = binary
        self.database = database
        self.port = free_port()
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            [binary, '--db', database, '--port', str(self.port),
             *(cache_arguments or ['--cache', 'off'])],
            stdout=self.log, stderr=self.log)


def detail_is_seeded(server):
    status, content_type, body = server.request('GET', '/products/1')
    expect(status, 200, '상품 상세 상태')
    expect(content_type, 'application/json', '상품 상세 형식')
    expect(json.loads(body), {'id': 1, 'name': '상품 1', 'price': 48271}, 'seed 상품')


def origin_rejects_invalid_data(server):
    # 공개 API 밖의 원본 손상은 SQLite 연결에서 주입하고 결과는 HTTP로 확인합니다.
    with sqlite3.connect(server.database) as database:
        database.execute('PRAGMA ignore_check_constraints=ON')
        database.execute("UPDATE products SET name='' WHERE id=2")
    status, _, body = server.request('GET', '/products/2')
    expect((status, json.loads(body)), (503, {'error': 'origin_unavailable'}), '잘못된 원본')


def read_json(server, path):
    status, content_type, body = server.request('GET', path)
    expect(content_type, 'application/json', 'JSON Content-Type')
    return status, json.loads(body)


def errors_follow_contract(server):
    expect(read_json(server, '/products/10001'),
           (404, {'error': 'product_not_found'}), '없는 상품')
    for path in ['/products/0', '/products/-1', '/products/1.0',
                 '/products/2147483648', '/products/999999999999999999999',
                 '/products/', '/products/1/extra', '/products/%00',
                 '/products/%GG', '/products/1?x=1', '/stats?x=1']:
        expect(read_json(server, path), (400, {'error': 'invalid_request'}), path)
    expect(read_json(server, '/unknown'), (404, {'error': 'route_not_found'}), '없는 경로')
    status, _, body = server.request('GET', '/products/1', body=b'x')
    expect((status, json.loads(body)), (400, {'error': 'invalid_request'}), 'GET body')
    connection = http.client.HTTPConnection('127.0.0.1', server.port, timeout=5)
    try:
        for path, allow in [('/products/1', 'GET, PUT, DELETE'),
                            ('/stats', 'GET'), ('/products', 'GET')]:
            connection.request('POST', path)
            response = connection.getresponse()
            expect((response.status, response.getheader('Allow'), json.loads(response.read())),
                   (405, allow, {'error': 'method_not_allowed'}), '미지원 메서드')
    finally:
        connection.close()


def stats_count_valid_reads(server):
    _, before = read_json(server, '/stats')
    expect(read_json(server, '/stats'), (200, before), '통계는 원본을 읽지 않음')
    read_json(server, '/products/1')
    read_json(server, '/products/10001')
    read_json(server, '/products/0')
    _, after = read_json(server, '/stats')
    expect(after, {**before, 'product_gets': before['product_gets'] + 2,
                   'origin_queries': before['origin_queries'] + 2,
                   'cache_bypasses': before['cache_bypasses'] + 2}, '유효한 상품 GET 수')
    expect(after['cache_state'], 'off', '캐시 상태')


def origin_lock_recovers(server):
    _, before = read_json(server, '/stats')
    database = sqlite3.connect(server.database)
    try:
        database.execute('BEGIN EXCLUSIVE')
        expect(read_json(server, '/products/1'),
               (503, {'error': 'origin_unavailable'}), '잠긴 원본')
    finally:
        database.rollback()
        database.close()
    detail_is_seeded(server)
    _, after = read_json(server, '/stats')
    expect(after['origin_queries'], before['origin_queries'] + 2, '실패와 회복 조회 수')
    expect(after['origin_failures'], before['origin_failures'] + 1, '실패 조회 수')


def origin_checks_name_and_price(server):
    cases = [('', 0), ('x' * 121, 0), ('a\x00b', 0), ('ok', -1),
             ('ok', 1000000001), ('ok', 1.5), (b'\xff', 0),
             (b'\xc0\xaf', 0), (b'\xed\xa0\x80', 0),
             (b'\xf4\x90\x80\x80', 0), (b'\xe2\x82', 0)]
    with sqlite3.connect(server.database) as database:
        database.execute('PRAGMA ignore_check_constraints=ON')
        for name, price in cases:
            if isinstance(name, bytes):
                database.execute('UPDATE products SET name=CAST(? AS TEXT),price=? WHERE id=2',
                                 (name, price))
            else:
                database.execute('UPDATE products SET name=?,price=? WHERE id=2', (name, price))
            database.commit()
            expect(read_json(server, '/products/2'),
                   (503, {'error': 'origin_unavailable'}), '범위 또는 UTF-8 위반')
        for name, price in [('한' * 40, 0), ('줄\n따옴표"\\😀', 1000000000)]:
            database.execute('UPDATE products SET name=?,price=? WHERE id=2', (name, price))
            database.commit()
            expect(read_json(server, '/products/2'),
                   (200, {'id': 2, 'name': name, 'price': price}), '유효한 경계와 JSON escaping')


def encoded_id_and_last_seeded_product(server):
    expect(read_json(server, '/products/%31'), read_json(server, '/products/1'), 'id 한 번 디코딩')
    status, last = read_json(server, '/products/10000')
    expect((status, last['id'], last['name']), (200, 10000, '상품 10000'), '기본 상품 수')


def command_check(binary, arguments, status):
    result = subprocess.run([binary, *arguments], capture_output=True, timeout=10)
    expect(result.returncode, status, '실행 종료 코드: ' + repr(arguments))
    output = (result.stdout + result.stderr).decode('utf-8', 'replace')
    for mark in SANITIZER_MARKS:
        expect(mark in output, False, 'sanitizer 오류: ' + output)


def stopped_check(server):
    exit_code = server.stop()
    output = server.output()
    server.log.close()
    expect(exit_code, 0, '데모 종료 코드: ' + output)
    for mark in SANITIZER_MARKS:
        expect(mark in output, False, 'sanitizer 오류: ' + output)


def lifecycle_and_cli(server):
    binary = server.binary
    command_check(binary, ['--db', server.database, '--port', str(server.port), '--cache', 'off'], 1)
    with tempfile.TemporaryDirectory() as directory:
        database = os.path.join(directory, 'fresh.sqlite')
        command_check(binary, ['--db', database, '--port', '8080', '--cache', 'off'], 1)
        with open(database, 'wb') as empty:
            empty.write(b'')
        command_check(binary, ['--db', database, '--port', '8080', '--cache', 'off'], 1)
        for arguments in [[], ['--db', database, '--seed', '0'],
                          ['--db', database, '--seed', '1', '--count', '0'],
                          ['--db', database, '--seed', '2147483647'],
                          ['--db', database, '--port', '65536', '--cache', 'off'],
                          ['--db', database, '--port', '8080', '--cache', 'invalid'],
                          ['--db', database, '--port', '8080', '--cache', 'off', '--port', '8081']]:
            command_check(binary, arguments, 2)
        command_check(binary, ['--db', database, '--seed', '1', '--count', '3'], 0)
        command_check(binary, ['--db', database, '--seed', '2', '--count', '2'], 0)
        prepared = Demo(binary, database)
        try:
            prepared.wait_until_listening()
            expect(read_json(prepared, '/products/1'),
                   (200, {'id': 1, 'name': '상품 1', 'price': 96542}), '지정 seed로 교체')
            expect(read_json(prepared, '/products/3'),
                   (404, {'error': 'product_not_found'}), '지정 상품 수로 교체')
            prepared.process.send_signal(signal.SIGINT)
            expect(prepared.process.wait(timeout=10), 0, 'SIGINT 종료')
        finally:
            stopped_check(prepared)


TESTS = [('정해진 seed의 상품 상세를 JSON으로 반환한다', detail_is_seeded),
         ('범위를 벗어난 원본 상품을 정상 응답으로 내보내지 않는다', origin_rejects_invalid_data),
         ('잘못된 요청과 없는 상품과 미지원 메서드를 구분한다', errors_follow_contract),
         ('통계는 유효한 상품 GET과 원본 조회만 센다', stats_count_valid_reads),
         ('원본 잠금으로 실패한 뒤 같은 프로세스에서 회복한다', origin_lock_recovers),
         ('상품 이름과 가격의 범위와 UTF-8을 검사한다', origin_checks_name_and_price),
         ('인코딩한 id와 기본 10000개 상품을 조회한다', encoded_id_and_last_seeded_product),
         ('준비와 시작 실패와 신호 종료에서 자원을 정리한다', lifecycle_and_cli)]


def main():
    binary = os.path.abspath(sys.argv[1])
    with tempfile.TemporaryDirectory() as directory:
        database = os.path.join(directory, 'products.sqlite')
        command_check(binary, ['--db', database, '--seed', '1'], 0)
        server = Demo(binary, database)
        try:
            server.wait_until_listening()
            for name, test in TESTS:
                test(server)
                print('PASS:', name)
        finally:
            stopped_check(server)
    print(f'cache functional: {len(TESTS)}개 통과')


if __name__ == '__main__':
    main()
