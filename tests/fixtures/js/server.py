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
    preflight_headers = {}
    actual_requests = {}
    actual_origins = {}
    actual_cookies = {}
    preflight_lock = threading.Lock()

    def end_headers(self):
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Connection", "close")
        super().end_headers()
        self.close_connection = True

    def cors(self, credentials=False, wildcard=False, origin=None):
        allowed = self.headers.get("Origin", "*") if origin is None else origin
        self.send_header("Access-Control-Allow-Origin", "*" if wildcard else allowed)
        if credentials:
            self.send_header("Access-Control-Allow-Credentials", "true")
        self.send_header("Vary", "Origin")
        self.send_header("Access-Control-Expose-Headers", "X-Exposed")

    def send_json(self, obj, cors=False, status=200, cache=False):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("X-Exposed", "visible")
        self.send_header("X-Private", "must-not-be-visible-cross-origin")
        if cache:
            self.send_header("Cache-Control", "public, max-age=3600")
            self.send_header("ETag", '"fixture-cache"')
            self.send_header("Last-Modified", "Wed, 01 Jan 2020 00:00:00 GMT")
        if cors:
            self.cors()
        self.end_headers()
        if self.command == "HEAD":
            return
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # An abort is an expected browser fixture outcome.

    def do_OPTIONS(self):
        # Actual Fetch OPTIONS is not a CORS preflight. Only the browser's
        # Access-Control-Request-Method field identifies a preflight request.
        if not self.headers.get("Access-Control-Request-Method"):
            if not self.api():
                self.send_json({"error": "OPTIONS is only supported by /api endpoints"}, status=405)
            return
        parsed = urlsplit(self.path)
        path = parsed.path
        query = parse_qs(parsed.query)
        with self.preflight_lock:
            self.preflights[self.path] = self.preflights.get(self.path, 0) + 1
            self.preflight_cookies[self.path] = self.headers.get("Cookie", "")
            self.preflight_headers[self.path] = self.headers.get("Access-Control-Request-Headers", "")
        self.send_response(204 if path.startswith("/api/") and path != "/api/no-cors" else 403)
        self.send_header("Content-Length", "0")
        if path != "/api/no-cors":
            self.cors(credentials=path.startswith("/api/cookie"))
            if query.get("allow_methods") != ["omit"]:
                self.send_header("Access-Control-Allow-Methods", query.get("allow_methods", ["GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS"])[0])
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
        with self.preflight_lock:
            methods = self.actual_requests.setdefault(self.path, {})
            methods[self.command] = methods.get(self.command, 0) + 1
            self.actual_origins.setdefault(self.path, []).append(self.headers.get("Origin", ""))
            self.actual_cookies.setdefault(self.path, []).append(self.headers.get("Cookie", ""))
        if path == "/api/observations":
            target = query.get("path", [""])[0]
            with self.preflight_lock:
                methods = dict(self.actual_requests.get(target, {}))
                preflight_count = self.preflights.get(target, 0)
                origins = list(self.actual_origins.get(target, []))
                cookies = list(self.actual_cookies.get(target, []))
            self.send_json({"actual_count": sum(methods.values()), "methods": methods,
                            "preflight_count": preflight_count, "origins": origins,
                            "cookies": cookies}, cors=True)
            return True
        if path == "/api/cors-overflow":
            body = b'{"ok":true}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.cors(credentials=True)
            # Legitimate CORS permission precedes the public HTTP snapshot
            # boundary; the conflicting singleton follows it. A parser must
            # not validate CORS against a silently incomplete header snapshot.
            if query.get("padding") in (["large"], ["twenty"]):
                # Many short fields fill the old snapshot to its last few
                # bytes. A single 5000-byte field would be skipped as a whole
                # and leave room for the trailing duplicate, missing the bug.
                for _ in range(960 if query.get("padding") == ["twenty"] else 320):
                    self.send_header("X-Snapshot-Padding", "p")
            else:
                self.send_header("X-Snapshot-Padding", "p" * 100)
            if query.get("duplicate") == ["origin"]:
                self.send_header("Access-Control-Allow-Origin", "https://disallowed.invalid")
            if query.get("duplicate") == ["credentials"]:
                self.send_header("Access-Control-Allow-Credentials", "false")
            self.send_header("X-Exposed", "visible-after-padding")
            self.send_header("X-Private", "still-private-after-padding")
            self.end_headers()
            if self.command != "HEAD":
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass
            return True
        if path == "/api/headers":
            # Large complete blocks, not an oversized individual line. The
            # worker's 16 KiB line bound is independent of its block bound.
            try:
                size = min(128 * 1024, max(0, int(query.get("bytes", ["6000"])[0])))
            except ValueError:
                size = 6000
            body = b"headers-ok"
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.cors(credentials=True)
            # Queue a cookie before the overflowing block. A failed response
            # must roll back its privileged cookie events, not partially apply.
            if query.get("cookie") == ["yes"]:
                self.send_header("Set-Cookie", "headeroverflow=not-applied; Path=/")
            else:
                self.send_header("Set-Cookie", "headersecret=not-visible; HttpOnly; Path=/")
            for index, offset in enumerate(range(0, size, 6000)):
                self.send_header(f"X-Header-Chunk-{index:02d}",
                                 chr(ord("a") + index % 26) * min(6000, size - offset))
            self.send_header("X-Header-End", "complete-after-padding")
            self.send_header("Set-Cookie2", "legacysecret=not-visible")
            self.end_headers()
            if self.command != "HEAD":
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass
            return True
        if path == "/api/status":
            # Preserve an actual HTTP reason phrase all the way through the
            # worker wire metadata; do not infer statusText from a status map.
            self.send_response(202, "Fixture Accepted")
            self.cors()
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", "8")
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(b"accepted")
            return True
        if path in ("/api/cookie", "/api/cookie-redirect"):
            with self.preflight_lock:
                preflight_count = self.preflights.get(self.path, 0)
                preflight_cookie = self.preflight_cookies.get(self.path, "")
            body = json.dumps({"cookie": self.headers.get("Cookie", ""),
                               "origin": self.headers.get("Origin", ""),
                               "preflight_cookie": preflight_cookie,
                               "preflight_count": preflight_count}).encode("utf-8")
            redirect = path.endswith("-redirect")
            self.send_response(302 if redirect else 200)
            self.cors(credentials=query.get("credentials", ["yes"])[0] == "yes",
                      wildcard=query.get("cors", [""])[0] == "wildcard",
                      origin=query.get("allow_origin", [None])[0])
            self.send_header("Content-Type", "application/json")
            self.send_header("X-Exposed", "visible")
            self.send_header("X-Private", "must-not-be-visible-after-cors-redirect")
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
            if self.command == "HEAD":
                return True
            try:
                self.wfile.write(body)
            except (BrokenPipeError, ConnectionResetError):
                pass
            return True
        if path in ("/api/redirect", "/api/infinite-redirect"):
            target = self.path if path.endswith("infinite-redirect") else query.get("url", ["/api/json"])[0]
            try:
                status = int(query.get("status", ["302"])[0])
            except ValueError:
                status = 302
            if status not in (301, 302, 303, 307, 308):
                self.send_json({"error": "unsupported redirect status"}, status=400)
                return True
            self.send_response(status)
            self.cors(credentials=query.get("credentials") == ["yes"])
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
            if self.command == "HEAD":
                return True
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
            if self.command == "HEAD":
                return True
            try:
                # Deliberately fragmented HTTP writes; pipes fragment this again inside Nocturne.
                for offset in range(0, len(body), 997):
                    self.wfile.write(body[offset:offset + 997])
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            return True
        if path not in ("/api/json", "/api/echo", "/api/echo-raw", "/api/cors", "/api/cors-wildcard",
                        "/api/no-cors", "/api/slow", "/api/request-info", "/api/cache"):
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
            if self.command == "HEAD":
                return True
            try:
                for offset in range(0, len(data), 997):
                    self.wfile.write(data[offset:offset + 997])
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            return True
        with self.preflight_lock:
            preflight_count = self.preflights.get(self.path, 0)
            preflight_headers = self.preflight_headers.get(self.path, "")
            actual_count = sum(self.actual_requests.get(self.path, {}).values())
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
                "preflight_headers": preflight_headers,
                "actual_count": actual_count,
                "cache_control": self.headers.get("Cache-Control", ""),
                "pragma": self.headers.get("Pragma", ""),
                "cache_control_fields": len(self.headers.get_all("Cache-Control", [])),
                "pragma_fields": len(self.headers.get_all("Pragma", [])),
                "if_none_match": self.headers.get("If-None-Match", ""),
                "authorization": bool(self.headers.get("Authorization")),
                "content_type": self.headers.get("Content-Type", ""),
                "content_encoding": self.headers.get("Content-Encoding", ""),
                "content_language": self.headers.get("Content-Language", ""),
                "content_location": self.headers.get("Content-Location", ""),
                "body_length": len(data),
            },
            cors=path != "/api/no-cors",
            cache=path == "/api/cache",
        )
        return True

    def do_GET(self):
        if not self.api():
            super().do_GET()

    def do_POST(self):
        if not self.api():
            self.send_json({"error": "POST is only supported by /api endpoints"}, status=405)

    def do_HEAD(self):
        if not self.api():
            super().do_HEAD()

    def do_PUT(self):
        if not self.api():
            self.send_json({"error": "PUT is only supported by /api endpoints"}, status=405)

    def do_PATCH(self):
        if not self.api():
            self.send_json({"error": "PATCH is only supported by /api endpoints"}, status=405)

    def do_DELETE(self):
        if not self.api():
            self.send_json({"error": "DELETE is only supported by /api endpoints"}, status=405)


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
