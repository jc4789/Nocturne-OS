"""Exercise the DNS fixture's UDP/TCP/control protocol, not guest TLS success."""
import socket
import struct
import threading
from dns_server import create_servers, question, wire_name


def receive(sock, count):
    result = b""
    while len(result) < count:
        chunk = sock.recv(count - len(result))
        if not chunk:
            raise AssertionError("short DNS TCP response")
        result += chunk
    return result


def query(port, name, tcp=False):
    packet = struct.pack("!HHHHHH", 42, 0x0100, 1, 0, 0, 0) + wire_name(name) + struct.pack("!HH", 1, 1)
    if tcp:
        with socket.create_connection(("127.0.0.1", port), timeout=2) as peer:
            peer.sendall(struct.pack("!H", len(packet)) + packet)
            reply = receive(peer, struct.unpack("!H", receive(peer, 2))[0])
    else:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
            peer.settimeout(2)
            peer.sendto(packet, ("127.0.0.1", port))
            reply = peer.recvfrom(65535)[0]
    _, flags, _, count, _, _ = struct.unpack("!HHHHHH", reply[:12])
    _, q = question(reply)
    position = 12 + len(q)
    addresses, ttls = [], []
    for _ in range(count):
        assert reply[position:position + 2] == b"\xc0\x0c"
        kind, cls, ttl, length = struct.unpack("!HHIH", reply[position + 2:position + 12])
        position += 12
        if kind == cls == 1:
            addresses.append(reply[position:position + length])
            ttls.append(ttl)
        position += length
    assert position == len(reply)
    return flags, addresses, ttls


def main():
    servers = create_servers(forward_external=True)
    fixture = servers[0].fixture
    external_queries = []

    def fixture_upstream(name):
        external_queries.append(name)
        if name == "missing.public.example":
            raise socket.gaierror(socket.EAI_NONAME, "absent")
        if name == "temporary.public.example":
            raise socket.gaierror(socket.EAI_AGAIN, "temporary")
        return [b"\xcb\x00\x71\x29"]

    # Only fixture forwarding is substituted. Product ns_open/kernel/TLS are
    # exercised by the separate real QEMU selectors, never by this host test.
    fixture.external_addresses = fixture_upstream
    for server in servers:
        threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        port = servers[0].server_address[1]
        flags, addresses, _ = query(port, "multi.fixture.test")
        assert flags == 0x8180 and len(addresses) == 3
        assert query(port, "tc.fixture.test")[0] & 0x0200
        assert len(query(port, "tc.fixture.test", tcp=True)[1]) == 2
        assert query(port, "cache-self.fixture.test")[0] == 0x8180
        assert query(port, "cache-self.fixture.test")[0] & 15 == 5
        for name, status in (("missing", 3), ("servfail", 2), ("refused", 5)):
            assert query(port, name + ".fixture.test")[0] & 15 == status
        assert query(port, "unknown.fixture.test")[0] & 15 == 3
        assert not external_queries
        assert list(reversed(query(port, "stream-failover.fixture.test")[1])) == [b"\x0a\x00\x02\xfa", b"\x0a\x00\x02\x02"]
        assert list(reversed(query(port, "stream-all-bad.fixture.test")[1])) == [b"\x0a\x00\x02\xfa", b"\x0a\x00\x02\xfb"]
        assert query(port, "example.com")[1] == [b"\xcb\x00\x71\x29"]
        assert query(port, "missing.public.example")[0] & 15 == 3
        assert query(port, "temporary.public.example")[0] & 15 == 2
        assert query(port, "nghttp2.org")[1] == [b"\xcb\x00\x71\x29"]
        assert query(port, "arm-tls-failover.fixture.test")[2] == [0]
        _, addresses, ttls = query(port, "nghttp2.org", tcp=True)
        assert list(reversed(addresses)) == [b"\x0a\x00\x02\xfa", b"\xcb\x00\x71\x29"] and ttls == [0, 0]
        assert query(port, "disarm-tls-failover.fixture.test")[2] == [0]
        assert query(port, "nghttp2.org")[1] == [b"\xcb\x00\x71\x29"]
        print("dns fixture UDP/TCP forwarding and scoped TLS candidates: 20 checks, 0 failed")
    finally:
        for server in servers:
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    main()
