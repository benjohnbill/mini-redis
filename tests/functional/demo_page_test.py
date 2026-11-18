#!/usr/bin/env python3
"""데모 페이지(demo/page/)의 이름 생성과 요청 전달 서버를 확인합니다.

요청 전달 서버는 실제 HTTP 서버 두 개(전달 서버와 대역 백엔드)로 확인합니다.
"""
import http.client
import http.server
import json
import os
import sys
import tempfile
import threading
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..', '..', 'demo', 'page'))

from names import COMBINATIONS, product_name  # noqa: E402
from proxy import make_server  # noqa: E402


class ProductNameTest(unittest.TestCase):
    def test_같은_id는_항상_같은_이름을_받는다(self):
        self.assertEqual(product_name(12345), product_name(12345))

    def test_조합_수만큼의_연속된_id는_모두_다른_이름을_받는다(self):
        names = {product_name(i) for i in range(1, COMBINATIONS + 1)}
        self.assertEqual(len(names), COMBINATIONS)

    def test_조합_수만큼_떨어진_id는_같은_이름을_받는다(self):
        self.assertEqual(product_name(7), product_name(7 + COMBINATIONS))

    def test_이웃한_id는_다른_이름을_받는다(self):
        self.assertNotEqual(product_name(1), product_name(2))

    def test_모든_이름이_데모_백엔드의_이름_규칙을_지킨다(self):
        for i in range(1, COMBINATIONS + 1):
            encoded = product_name(i).encode("utf-8")
            self.assertTrue(1 <= len(encoded) <= 120, encoded)
            self.assertNotIn(b"\0", encoded)


class FakeBackend(http.server.BaseHTTPRequestHandler):
    """요청을 기록하고, 받은 내용을 JSON으로 돌려주는 실제 HTTP 서버입니다."""

    received = []

    def handle_one(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        FakeBackend.received.append(
            (self.command, self.path, self.headers.get("Content-Type"), body))
        status = 404 if self.path.startswith("/missing") else 200
        payload = json.dumps({"path": self.path}).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    do_GET = do_PUT = handle_one

    def log_message(self, *args):
        pass


def start(server):
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    return server


class ProxyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.backend = start(
            http.server.HTTPServer(("127.0.0.1", 0), FakeBackend))
        cls.static_dir = tempfile.mkdtemp()
        with open(os.path.join(cls.static_dir, "index.html"), "w") as page:
            page.write("<h1>demo</h1>")
        backend_port = cls.backend.server_address[1]
        cls.proxy = start(make_server(
            0, {"off": backend_port, "on": backend_port}, cls.static_dir))

    @classmethod
    def tearDownClass(cls):
        cls.proxy.shutdown()
        cls.backend.shutdown()

    def request(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.proxy.server_address[1])
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        data = response.read()
        connection.close()
        return response, data

    def setUp(self):
        FakeBackend.received.clear()

    def test_앞부분을_뗀_경로와_query를_백엔드에_전달한다(self):
        response, data = self.request("GET", "/off/products?q=%EB%AC%B4%EC%84%A0")
        self.assertEqual(response.status, 200)
        self.assertEqual(FakeBackend.received[0][1], "/products?q=%EB%AC%B4%EC%84%A0")
        self.assertEqual(json.loads(data), {"path": "/products?q=%EB%AC%B4%EC%84%A0"})

    def test_백엔드의_상태_코드를_그대로_돌려준다(self):
        response, _ = self.request("GET", "/on/missing")
        self.assertEqual(response.status, 404)

    def test_백엔드를_기다린_시간을_ms_헤더로_알려준다(self):
        response, _ = self.request("GET", "/on/stats")
        self.assertGreaterEqual(float(response.getheader("X-Backend-Ms")), 0.0)

    def test_PUT의_body와_Content_Type을_전달한다(self):
        body = b'{"name":"a","price":1}'
        self.request("PUT", "/on/products/3", body=body,
                     headers={"Content-Type": "application/json"})
        self.assertEqual(FakeBackend.received[0],
                         ("PUT", "/products/3", "application/json", body))

    def test_모르는_앞부분은_백엔드에_보내지_않고_404를_돌려준다(self):
        response, _ = self.request("GET", "/both/products")
        self.assertEqual(response.status, 404)
        self.assertEqual(FakeBackend.received, [])

    def test_루트_경로는_페이지를_돌려준다(self):
        response, data = self.request("GET", "/")
        self.assertEqual(response.status, 200)
        self.assertEqual(data, b"<h1>demo</h1>")


if __name__ == '__main__':
    unittest.main()
