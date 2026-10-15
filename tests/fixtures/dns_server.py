"""Local DNS UDP/TCP fixture for the native resolver integration regression."""
import socket
import socketserver
import struct
import threading
import time


def wire_name(name):
    return b"".join(bytes((len(label),)) + label.encode("ascii") for label in name.split(".")) + b"\0"


def question(packet):
    offset = 12
    labels = []
    while offset < len(packet):
        size = packet[offset]
        offset += 1
        if not size:
            return ".".join(labels).lower(), packet[12:offset + 4]
        if size > 63 or offset + size > len(packet):
            raise ValueError("invalid question")
        labels.append(packet[offset:offset + size].decode("ascii"))
        offset += size
    raise ValueError("missing question")


class DnsFixture:
    def __init__(self, forward_external=False):
        self.lock = threading.Lock()
        self.counts = {}
        self.tcp_queries = 0
        self.forward_external = forward_external
        self.external_cache = {}
        self.tls_failover = False

    def external_addresses(self, name):
        # Never ask the host resolver to interpret fixture-owned test names.
        if name.endswith(".fixture.test"):
            raise ValueError("fixture names must have an explicit DNS case")
        now = time.monotonic()
        with self.lock:
            saved = self.external_cache.get(name)
            if saved and saved[0] > now:
                return saved[1]
        addresses = []
        for result in socket.getaddrinfo(name, None, socket.AF_INET, socket.SOCK_STREAM):
            address = socket.inet_aton(result[4][0])
            if address not in addresses:
                addresses.append(address)
        with self.lock:
            self.external_cache[name] = (now + 60, addresses)
        return addresses

    def reply(self, packet, tcp):
        name, q = question(packet)
        flags, answers = 0x8180, []

        def rr(owner, kind, payload, ttl=60):
            encoded = b"\xc0\x0c" if owner == name else wire_name(owner)
            return encoded + struct.pack("!HHIH", kind, 1, ttl, len(payload)) + payload

        if tcp:
            with self.lock:
                self.tcp_queries += 1
        if name == "stall.fixture.test":
            return None
        if name == "tcstall.fixture.test":
            if tcp:
                time.sleep(5)
                return None
            flags = 0x8380
        elif name == "tc.fixture.test" and not tcp:
            flags = 0x8380
        elif name == "missing.fixture.test":
            flags = 0x8183
        elif name == "servfail.fixture.test":
            flags = 0x8182
        elif name == "refused.fixture.test":
            flags = 0x8185
        elif name == "empty.fixture.test":
            pass
        elif name == "alias.fixture.test":
            answers = [rr(name, 5, wire_name("target.fixture.test")),
                       rr("unrelated.fixture.test", 1, b"\xde\xad\xbe\xef"),
                       rr("target.fixture.test", 1, b"\x0a\x00\x02\x02")]
        elif name.startswith("cache-"):
            with self.lock:
                count = self.counts.get(name, 0) + 1
                self.counts[name] = count
            if count > 1:
                flags = 0x8185
            else:
                answers = [rr(name, 1, b"\x0a\x00\x02\x02")]
        elif name == "multi.fixture.test":
            answers = [rr(name, 1, address) for address in
                       (b"\x0a\x00\x02\x02", b"\x0a\x00\x02\xfb", b"\x0a\x00\x02\xfa")]
        elif name == "tc.fixture.test":
            answers = [rr(name, 1, b"\x0a\x00\x02\x02"), rr(name, 1, b"\x0a\x00\x02\xfa")]
        elif name == "stream-failover.fixture.test":
            # Production DNS currently publishes matching A records in reverse
            # answer-chain order: the guest asserts the first actual candidate.
            answers = [rr(name, 1, b"\x0a\x00\x02\x02"), rr(name, 1, b"\x0a\x00\x02\xfa")]
        elif name == "stream-all-bad.fixture.test":
            answers = [rr(name, 1, b"\x0a\x00\x02\xfb"), rr(name, 1, b"\x0a\x00\x02\xfa")]
        elif name in {"arm-tls-failover.fixture.test", "disarm-tls-failover.fixture.test"}:
            with self.lock:
                self.tls_failover = name.startswith("arm-")
            answers = [rr(name, 1, b"\x0a\x00\x02\x02", 0)]
        elif self.forward_external and not name.endswith(".fixture.test"):
            try:
                addresses = self.external_addresses(name)
                with self.lock:
                    failover = self.tls_failover
                if name == "nghttp2.org" and failover:
                    # SNI and certificate verification still use the real host.
                    # Only this test fixture places an unreachable address first.
                    addresses = [b"\x0a\x00\x02\xfa"] + addresses
                # No resolver-cache entry may outlive the explicit TLS case's
                # mode change; other test groups keep their ordinary addresses.
                ttl = 0 if name == "nghttp2.org" else 60
                answers = [rr(name, 1, address, ttl) for address in reversed(addresses)]
            except socket.gaierror as error:
                absent = {socket.EAI_NONAME, getattr(socket, "EAI_NODATA", socket.EAI_NONAME)}
                flags = 0x8183 if error.errno in absent else 0x8182
        else:
            flags = 0x8183
        return packet[:2] + struct.pack("!HHHHH", flags, 1, len(answers), 0, 0) + q + b"".join(answers)


class DnsUdpHandler(socketserver.BaseRequestHandler):
    def handle(self):
        packet, sock = self.request
        try:
            reply = self.server.fixture.reply(packet, False)
            if reply is not None:
                sock.sendto(reply, self.client_address)
        except (OSError, ValueError, UnicodeError):
            pass


class DnsTcpHandler(socketserver.StreamRequestHandler):
    def handle(self):
        try:
            prefix = self.rfile.read(2)
            if len(prefix) != 2:
                return
            packet = self.rfile.read(struct.unpack("!H", prefix)[0])
            reply = self.server.fixture.reply(packet, True)
            if reply is None:
                return
            # Separate writes exercise length-prefix and body short reads.
            self.wfile.write(struct.pack("!H", len(reply))[:1])
            self.wfile.flush()
            self.wfile.write(struct.pack("!H", len(reply))[1:] + reply[:7])
            self.wfile.flush()
            self.wfile.write(reply[7:])
        except (OSError, ValueError, UnicodeError):
            pass


def create_servers(forward_external=False):
    fixture = DnsFixture(forward_external)
    tcp = socketserver.ThreadingTCPServer(("127.0.0.1", 0), DnsTcpHandler)
    tcp.daemon_threads = True
    try:
        udp = socketserver.ThreadingUDPServer(tcp.server_address, DnsUdpHandler)
    except BaseException:
        tcp.server_close()
        raise
    udp.daemon_threads = True
    tcp.fixture = udp.fixture = fixture
    return tcp, udp
