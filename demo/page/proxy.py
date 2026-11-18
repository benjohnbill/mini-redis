"""데모 페이지를 내려주고, /off/와 /on/ 요청을 두 데모 백엔드에 전달합니다.

데모 백엔드에는 CORS 헤더가 없으므로 브라우저가 직접 부를 수 없습니다. 이
서버가 같은 origin에서 페이지와 요청을 함께 맡습니다. 응답의 X-Backend-Ms는
이 서버가 백엔드에 요청을 보내고 응답을 다 받을 때까지 걸린 시간입니다.
브라우저와 이 서버 사이의 시간은 넣지 않아 양쪽에 같은 기준으로 잽니다.

make demo-page가 run.sh를 거쳐 부릅니다.
"""

import argparse
import http.client
import http.server
import os
import time

FORWARDED_HEADERS = ("Content-Type",)
BACKEND_TIMEOUT_SECONDS = 10


def make_server(port, backend_ports, static_dir):
    handler = type("DemoHandler", (DemoHandler,), {
        "backend_ports": backend_ports, "static_dir": static_dir})

    return http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)


class DemoHandler(http.server.BaseHTTPRequestHandler):
    backend_ports = {}
    static_dir = "."

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self.send_page()
            return
        self.forward()

    def do_PUT(self):
        self.forward()

    def send_page(self):
        with open(os.path.join(self.static_dir, "index.html"), "rb") as page:
            body = page.read()
        self.reply(200, "text/html; charset=utf-8", body, {})

    def forward(self):
        side, _, rest = self.path.lstrip("/").partition("/")
        if side not in self.backend_ports:
            self.reply(404, "application/json", b'{"error":"unknown_side"}', {})
            return
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else None
        headers = {name: self.headers[name] for name in FORWARDED_HEADERS
                   if self.headers.get(name)}
        status, content_type, data, elapsed_ms = backend_request(
            self.backend_ports[side], self.command, "/" + rest, body, headers)
        self.reply(status, content_type, data,
                   {"X-Backend-Ms": f"{elapsed_ms:.3f}"})

    def reply(self, status, content_type, body, extra_headers):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for name, value in extra_headers.items():
            self.send_header(name, value)
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        pass


def backend_request(port, method, path, body, headers):
    connection = http.client.HTTPConnection(
        "127.0.0.1", port, timeout=BACKEND_TIMEOUT_SECONDS)
    try:
        started = time.perf_counter()
        connection.request(method, path, body=body, headers=headers)
        response = connection.getresponse()
        data = response.read()
        elapsed_ms = (time.perf_counter() - started) * 1000
    finally:
        connection.close()
    content_type = response.getheader("Content-Type") or "application/json"

    return response.status, content_type, data, elapsed_ms


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--off", type=int, required=True)
    parser.add_argument("--on", type=int, required=True)
    options = parser.parse_args()
    static_dir = os.path.dirname(os.path.abspath(__file__))
    server = make_server(
        options.port, {"off": options.off, "on": options.on}, static_dir)
    print(f"demo page: http://127.0.0.1:{options.port}/", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
