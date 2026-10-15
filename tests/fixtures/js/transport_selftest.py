"""Validate the host fixtures independently before interpreting guest failures."""
import http.client
import socket
import threading
from functools import partial
from pathlib import Path
from h2.config import H2Configuration
from h2.connection import H2Connection
from h2.events import DataReceived, StreamEnded
from h2_server import create_server
from server import Handler, ThreadingHTTPServer


def main():
    http_server = ThreadingHTTPServer(("127.0.0.1", 0), partial(Handler, directory=str(Path(__file__).parent)))
    h2 = create_server()
    for server in (http_server, h2):
        threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        c = http.client.HTTPConnection(*http_server.server_address, timeout=3)
        ids = []
        for method, kind in (("GET", "plain"), ("HEAD", "head"), ("GET", "chunked"), ("GET", "plain")):
            c.request(method, "/api/net8-http?kind=" + kind)
            response = c.getresponse()
            assert response.status == 200
            ids.append(response.getheader("X-Connection-ID"))
            body = response.read()
            assert (not body) if method == "HEAD" else b'"connection"' in body
        assert len(set(ids)) == 1
        c.close()
        s = socket.create_connection(h2.server_address, timeout=3)
        client = H2Connection(config=H2Configuration(client_side=True))
        client.initiate_connection()
        for stream in (1, 3):
            client.send_headers(stream, [(":method", "GET"), (":scheme", "http"),
                                        (":authority", "localhost"), (":path", "/multiplex")], end_stream=True)
        s.sendall(client.data_to_send())
        done, body = set(), {}
        while len(done) != 2:
            for event in client.receive_data(s.recv(65536)):
                if isinstance(event, DataReceived):
                    body[event.stream_id] = body.get(event.stream_id, b"") + event.data
                    client.acknowledge_received_data(event.flow_controlled_length, event.stream_id)
                elif isinstance(event, StreamEnded):
                    done.add(event.stream_id)
            output = client.data_to_send()
            if output:
                s.sendall(output)
        assert all(b'"connection"' in value for value in body.values())
        s.close()
        print("transport fixture: HTTP1 reuse/HEAD/trailers and H2 multiplex passed")
    finally:
        for server in (http_server, h2):
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
