"""Manual Nocturne browser fixtures; no external Python packages.

python tests/fixtures/js/server.py
Open http://<host-address>:8000/index.html from Nocturne, not the guest's loopback.
Ports 8000 and 8001 deliberately provide different HTTP origins.
"""

import argparse
import json
import mimetypes
import threading
import time
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit


mimetypes.add_type("text/javascript", ".js")
mimetypes.add_type("text/javascript", ".mjs")


class Handler(SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    preflights = {}
    preflight_cookies = {}
    preflight_lock = threading.Lock()

    def end_headers(self):
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Connection", "close")
        super().end_headers()
        self.close_connection = True

    def cors(self, credentials=False, wildcard=False):
        self.send_header("Access-Control-Allow-Origin", "*" if wildcard else self.headers.get("Origin", "*"))
        if credentials:
            self.send_header("Access-Control-Allow-Credentials", "true")
        self.send_header("Vary", "Origin")
        self.send_header("Access-Control-Expose-Headers", "X-Exposed")

    def send_json(self, obj, cors=False, status=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Exposed", "visible")
        self.send_header("X-Private", "must-not-be-visible-cross-origin")
        if cors:
            self.cors()
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # An abort is an expected browser fixture outcome.

    def do_OPTIONS(self):
        parsed = urlsplit(self.path)
        path = parsed.path
        query = parse_qs(parsed.query)
        with self.preflight_lock:
            self.preflights[self.path] = self.preflights.get(self.path, 0) + 1
            self.preflight_cookies[self.path] = self.headers.get("Cookie", "")
        self.send_response(204 if path.startswith("/api/") and path != "/api/no-cors" else 403)
        self.send_header("Content-Length", "0")
        if path != "/api/no-cors":
            self.cors(credentials=path.startswith("/api/cookie"))
            if query.get("allow_methods") != ["omit"]:
                self.send_header("Access-Control-Allow-Methods", "GET, POST")
            allowed = "*" if path == "/api/cors-wildcard" else self.headers.get("Access-Control-Request-Headers", "")
            if query.get("allow_headers") != ["omit"]:
                self.send_header("Access-Control-Allow-Headers", allowed)
        self.end_headers()

    def api(self):
        parsed = urlsplit(self.path)
        path = parsed.path
        if not path.startswith("/api/"):
            return False
        query = parse_qs(parsed.query)
        if path in ("/api/cookie", "/api/cookie-redirect"):
            with self.preflight_lock:
                preflight_count = self.preflights.get(self.path, 0)
                preflight_cookie = self.preflight_cookies.get(self.path, "")
            body = json.dumps({"cookie": self.headers.get("Cookie", ""),
                               "preflight_cookie": preflight_cookie,
                               "preflight_count": preflight_count}).encode("utf-8")
            redirect = path.endswith("-redirect")
            self.send_response(302 if redirect else 200)
            self.cors(credentials=query.get("credentials", ["yes"])[0] == "yes",
                      wildcard=query.get("cors", [""])[0] == "wildcard")
            self.send_header("Content-Type", "application/json")
            if query.get("padding") == ["yes"]:
                self.send_header("X-Cookie-Test-Padding", "p" * 6000)
            for value in query.get("set", []):
                self.send_header("Set-Cookie", value)
            if query.get("long") == ["yes"]:
                self.send_header("Set-Cookie", "longcookie=" + "v" * 2400 + "; Path=/")
            if query.get("overlong") == ["yes"]:
                self.send_header("Set-Cookie", "overlong=" + "v" * 8200 + "; Path=/")
            if redirect:
                self.send_header("Location", query.get("url", ["/api/cookie"])[0])
                body = b""
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return True
        if path in ("/api/redirect", "/api/infinite-redirect"):
            target = self.path if path.endswith("infinite-redirect") else query.get("url", ["/api/json"])[0]
            self.send_response(302)
            self.cors()
            self.send_header("Location", target)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return True
        if path == "/api/slow":
            try:
                delay = min(35000, max(0, int(query.get("ms", ["5000"])[0])))
            except ValueError:
                delay = 5000
            time.sleep(delay / 1000)
        if path == "/api/oversize":
            length = 16 * 1024 * 1024 + 1
            self.send_response(200)
            self.cors()
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(length))
            self.end_headers()
            try:
                while length:
                    n = min(length, 65536)
                    self.wfile.write(b"x" * n)
                    length -= n
            except (BrokenPipeError, ConnectionResetError):
                pass
            return True
        if path in ("/api/pattern", "/api/module"):
            if path == "/api/module":
                body = b"export const value = 42;\n"
                mime = "text/javascript"
            else:
                try:
                    length = min(1024 * 1024, max(0, int(query.get("n", ["196613"])[0])))
                except ValueError:
                    length = 196613
                body = bytes((i * 37 + 11) & 255 for i in range(length))
                mime = "application/octet-stream"
            self.send_response(200)
            self.cors()
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                # Deliberately fragmented HTTP writes; pipes fragment this again inside Nocturne.
                for offset in range(0, len(body), 997):
                    self.wfile.write(body[offset:offset + 997])
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            return True
        if path not in ("/api/json", "/api/echo", "/api/echo-raw", "/api/cors", "/api/cors-wildcard",
                        "/api/no-cors", "/api/slow", "/api/request-info"):
            self.send_json({"error": "unknown fixture endpoint"}, status=404)
            return True
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = -1
        if length < 0 or length > 16 * 1024 * 1024:
            self.send_json({"error": "oversized request"}, status=413)
            return True
        data = self.rfile.read(length) if length else b""
        if path == "/api/echo-raw":
            self.send_response(200)
            self.cors()
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            try:
                for offset in range(0, len(data), 997):
                    self.wfile.write(data[offset:offset + 997])
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            return True
        with self.preflight_lock:
            preflight_count = self.preflights.get(self.path, 0)
        self.send_json(
            {
                "ok": True,
                "message": "Nocturne",
                "value": 42,
                "method": self.command,
                "query": query,
                "body": data.decode("utf-8", errors="replace"),
                "x_fixture": self.headers.get("X-Fixture", ""),
                "preflight_count": preflight_count,
                "authorization": bool(self.headers.get("Authorization")),
                "content_type": self.headers.get("Content-Type", ""),
            },
            cors=path != "/api/no-cors",
        )
        return True

    def do_GET(self):
        if not self.api():
            super().do_GET()

    def do_POST(self):
        if not self.api():
            self.send_json({"error": "POST is only supported by /api endpoints"}, status=405)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--ports", nargs="+", type=int, default=[8000, 8001])
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    servers = []
    try:
        for port in args.ports:
            server = ThreadingHTTPServer((args.bind, port), partial(Handler, directory=str(root)))
            server.daemon_threads = True
            servers.append(server)
            threading.Thread(target=server.serve_forever, daemon=True).start()
            print(f"Nocturne fixture: http://<host-address>:{port}/index.html", flush=True)
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass
    finally:
        for server in servers:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
