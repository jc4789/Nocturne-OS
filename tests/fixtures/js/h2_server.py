"""Loopback HTTP/2 prior-knowledge fixture; production uses the same codec via ALPN.

Requires the host's h2 package. No guest trust roots or TLS verification are changed.
The /multiplex responses wait for two simultaneous streams on ONE connection.
"""
import json
import socket
import socketserver
import time

from h2.config import H2Configuration
from h2.connection import H2Connection
from h2.events import DataReceived, RequestReceived, StreamEnded, StreamReset, WindowUpdated
from h2.settings import SettingCodes


class H2Handler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(25)
        connection = H2Connection(config=H2Configuration(client_side=False, header_encoding="utf-8"))
        connection.initiate_connection()
        self.request.sendall(connection.data_to_send())
        requests, pending, held = {}, {}, []
        delayed = []
        cancelled_stream = None
        cancelled_peer = None
        cancellation_started = False
        budget_stream = None
        budget_peer = None
        budget_started = False
        closing = False
        late_goaway = False

        def respond(stream):
            request = requests[stream]
            path = request["path"]
            if path.startswith("/reset"):
                connection.reset_stream(stream, error_code=8)
                return
            payload = (bytes(i % 251 for i in range(192 * 1024)) if path.startswith("/large") else
                       json.dumps({"stream": stream, "connection": self.client_address[1],
                                   "uploaded": request["uploaded"], "path": path}).encode())
            fields = [(":status", "200"),
                                             ("content-type", "application/octet-stream"),
                                             ("content-length", str(len(payload))),
                                             ("x-connection-id", str(self.client_address[1])),
                                             ("x-hpack-dynamic", "same-value-for-every-stream")]
            if path.startswith("/late-cancel-b"):
                fields.append(("x-late-trailer", "retained-after-cancel"))
            if path.startswith("/header-budget-b"):
                fields.append(("x-budget-synced", "retained-after-budget"))
            connection.send_headers(stream, fields)
            pending[stream] = [payload, 0, path.startswith("/trailers")]

        def raw_headers(stream, fields, huffman):
            def integer(value, bits, prefix):
                maximum = (1 << bits) - 1
                if value < maximum:
                    return bytes([prefix | value])
                result = bytearray([prefix | maximum])
                value -= maximum
                while value >= 128:
                    result.append((value & 127) | 128)
                    value >>= 7
                result.append(value)
                return bytes(result)

            blocks = []
            for name, value in fields:
                if len(value) < 65536:
                    blocks.append(connection.encoder.encode([(name, value)], huffman=huffman))
                    continue
                # hpack's generic Python encoder grows a giant integer for
                # every symbol. These repeated '~' values have a linear
                # fixture encoder, using that package's RFC Huffman table.
                assert value == "~" * len(value)
                encoded_name = name.encode()
                encoded_value = value.encode()
                if huffman:
                    coder = connection.encoder.huffman_coder
                    code, bits = coder.huffman_code_list[126], coder.huffman_code_list_lengths[126]
                    pattern = 0
                    for _ in range(8):
                        pattern = (pattern << bits) | code
                    encoded_value = pattern.to_bytes(bits, "big") * (len(value) // 8)
                    tail = len(value) % 8
                    if tail:
                        pattern = 0
                        for _ in range(tail):
                            pattern = (pattern << bits) | code
                        padding = (-tail * bits) % 8
                        encoded_value += ((pattern << padding) | ((1 << padding) - 1)).to_bytes((tail * bits + 7) // 8, "big")
                blocks.append(b"\x40" + integer(len(encoded_name), 7, 0) + encoded_name +
                              integer(len(encoded_value), 7, 128 if huffman else 0) + encoded_value)
                connection.encoder.header_table.add(encoded_name, value.encode())
            block = b"".join(blocks)
            frames = []
            for offset in range(0, len(block), 16384):
                part = block[offset:offset + 16384]
                flags = (1 if offset == 0 else 0) | (4 if offset + len(part) == len(block) else 0)
                frames.append(len(part).to_bytes(3, "big") + bytes([1 if offset == 0 else 9, flags]) + stream.to_bytes(4, "big") + part)
            print("H2 header-block stream=" + str(stream) + " encoded=" + str(len(block)) + " huffman=" + str(huffman), flush=True)
            self.request.sendall(connection.data_to_send() + b"".join(frames))

        try:
            while True:
                for stream in list(pending):
                    payload, offset, trailers = pending[stream]
                    while offset < len(payload):
                        size = min(connection.local_flow_control_window(stream),
                                   connection.max_outbound_frame_size, len(payload) - offset)
                        if size <= 0:
                            break
                        final = offset + size == len(payload)
                        connection.send_data(stream, payload[offset:offset + size],
                                             end_stream=final and not trailers)
                        offset += size
                    pending[stream][1] = offset
                    if offset == len(payload):
                        if trailers:
                            connection.send_headers(stream, [("x-native-trailer", "complete")], end_stream=True)
                        del pending[stream]
                if closing and not pending:
                    connection.close_connection()
                outgoing = connection.data_to_send()
                if outgoing:
                    self.request.sendall(outgoing)
                if closing and not pending:
                    return
                if late_goaway and not pending:
                    time.sleep(0.05)
                    connection.close_connection()
                    self.request.sendall(connection.data_to_send())
                    # Keep TCP open long enough that checkout discovers the
                    # peer's explicit last-stream ID, not just stale EOF.
                    time.sleep(0.3)
                    return
                incoming = self.request.recv(65536)
                if not incoming:
                    return
                for event in connection.receive_data(incoming):
                    if isinstance(event, RequestReceived):
                        headers = dict(event.headers)
                        requests[event.stream_id] = {"path": headers.get(":path", "/"), "uploaded": 0}
                        if headers.get(":path", "").startswith("/stall-upload"):
                            # Open H2 credit but stop reading TCP. This fills
                            # native transmit storage rather than merely
                            # waiting on an HTTP/2 stream window.
                            connection.update_settings({SettingCodes.INITIAL_WINDOW_SIZE: 16 * 1024 * 1024})
                            connection.increment_flow_control_window(16 * 1024 * 1024 - 65535)
                            self.request.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                            self.request.sendall(connection.data_to_send())
                            time.sleep(5)
                    elif isinstance(event, DataReceived):
                        requests[event.stream_id]["uploaded"] += len(event.data)
                        connection.acknowledge_received_data(event.flow_controlled_length, event.stream_id)
                    elif isinstance(event, StreamEnded):
                        path = requests[event.stream_id]["path"]
                        if path.startswith("/late-cancel-a"):
                            cancelled_stream = event.stream_id
                            connection.send_headers(event.stream_id, [(":status", "200"), ("content-length", "1")])
                        elif path.startswith("/late-cancel-b"):
                            cancelled_peer = event.stream_id
                        elif path.startswith("/header-budget-a"):
                            budget_stream = event.stream_id
                        elif path.startswith("/header-budget-b"):
                            budget_peer = event.stream_id
                        elif path.startswith("/header-expanded"):
                            # '~' takes 13 Huffman bits: its legal decoded
                            # value fits the 1 MiB field budget, while the
                            # encoded block exceeds 1 MiB. No wire-block cap.
                            raw_headers(event.stream_id, [(":status", "200"), ("content-length", "0"),
                                ("x-expanded", "~" * (700 * 1024))], True)
                        elif path.startswith("/multiplex"):
                            held.append(event.stream_id)
                            if len(held) == 2:
                                for stream in held:
                                    respond(stream)
                                held.clear()
                        elif path.startswith("/httpbin/delay/1"):
                            # The TLS API model must prove two requests are
                            # in flight before either response header exists.
                            delayed.append(event.stream_id)
                            if len(delayed) == 2:
                                print("H2 delayed in-flight streams=" + ",".join(map(str, delayed)), flush=True)
                                time.sleep(1)
                                for stream in delayed:
                                    respond(stream)
                                delayed.clear()
                        else:
                            respond(event.stream_id)
                            closing |= path.startswith("/goaway")
                            late_goaway |= path.startswith("/late-goaway")
                    elif isinstance(event, StreamReset):
                        pending.pop(event.stream_id, None)
                        if event.stream_id == cancelled_stream and cancelled_peer is not None:
                            # This block was already in flight when RST_STREAM
                            # crossed it. Encode on the SAME HPACK context;
                            # B's later headers reference the inserted entry.
                            block = connection.encoder.encode([("x-late-trailer", "retained-after-cancel")], huffman=False)
                            late = len(block).to_bytes(3, "big") + bytes([1, 5]) + cancelled_stream.to_bytes(4, "big") + block
                            self.request.sendall(connection.data_to_send() + late)
                            print("H2 cancelled-stream late HPACK trailer then peer=" + str(cancelled_peer), flush=True)
                            respond(cancelled_peer)
                    elif isinstance(event, WindowUpdated):
                        pass  # The next iteration resumes every unblocked stream fairly.
                if cancelled_stream is not None and cancelled_peer is not None and not cancellation_started:
                    cancellation_started = True
                    connection.send_data(cancelled_stream, b"x", end_stream=False)
                if budget_stream is not None and budget_peer is not None and not budget_started:
                    budget_started = True
                    huffman = "huffman" in requests[budget_stream]["path"]
                    # The giant incremental entry empties the table, then a
                    # small entry is inserted AFTER the refused field. B's
                    # indexed use proves the complete block was still read.
                    raw_headers(budget_stream, [(":status", "200"), ("x-too-large", "~" * (1024 * 1024 + 65536)),
                        ("x-budget-synced", "retained-after-budget")], huffman)
                    respond(budget_peer)
        except (ConnectionError, TimeoutError, OSError):
            return


class H2Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def create_server():
    return H2Server(("127.0.0.1", 0), H2Handler)
