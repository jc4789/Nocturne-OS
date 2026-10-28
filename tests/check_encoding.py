"""Host check of user/libc/web/web_encoding.c, the browser's one owner of the Encoding Standard,
and of Lexbor's URL percent-encoding that uses its encoders.

Builds tests/encoding_hosttest.c with Lexbor's core, encoding, unicode and URL modules and
compares it with a reference written here from the standard's algorithms (encoding.bs) and data
(indexes.json, encodings.json) in the standard's repository, and from the URL Standard's
"percent-encode after encoding":
- every label, with ASCII case and ASCII whitespace, and strings that are not labels;
- every BMP scalar value and a sample of the others through every encoder (encode or fail);
- every one- and two-byte input, EUC-JP's three-byte and gb18030's four-byte inputs and random
  inputs through every decoder, cut at several chunk sizes, in replacement and fatal mode and
  with TextDecoder's BOM drop; BOM sniffing;
- random strings encoded in html mode (forms), and as URL queries and urlencoded bytes the way
  WPT's encode-href and encode-form tests make them.
Not the guest: TextDecoder's JS, the HTML parser and form submission are outside it.
"""
from bisect import bisect_right
import ast
from collections import deque
from pathlib import Path
import json
import random
import re
import struct
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
spec = Path(sys.argv[1] if len(sys.argv) > 1 else
            r"D:\Programes\cloned repos\Browser specs\whatwg specs\encoding")
out = root / "build" / "encoding-host"
out.mkdir(parents=True, exist_ok=True)
exe = out / "encoding_hosttest.exe"
clang = next(p / "tools/msys64/ucrt64/bin/clang.exe" for p in (root, *root.parents)
             if (p / "tools/msys64/ucrt64/bin/clang.exe").exists())  # a worktree uses its repository's

modules = ("core", "encoding", "punycode", "unicode", "url")
lexbor = [str(root / line.split("+=")[1].strip())
          for line in (root / "third_party/lexbor/sources.mk").read_text("utf-8").splitlines()
          if "+=" in line and line.startswith("LXB_VENDOR_C") and
          any(f"/lexbor/{m}/" in line for m in modules)]
subprocess.run([str(clang), "-O2", "-w", "-DLEXBOR_STATIC",
                "-I" + str(root / "third_party/lexbor/source"), "-I" + str(root / "ports/lexbor/include"),
                str(root / "tests/encoding_hosttest.c"), str(root / "user/libc/web/web_encoding.c"),
                str(root / "ports/lexbor/memory.c"), *lexbor, "-o", str(exe)], check=True)

index = json.loads((spec / "indexes.json").read_text("utf-8"))
groups = json.loads((spec / "encodings.json").read_text("utf-8"))
NAMES = [e["name"] for g in groups for e in g["encodings"]]
LABELS = {label: e["name"] for g in groups for e in g["encodings"] for label in e["labels"]}
SINGLE = [e["name"] for g in groups if g["heading"] == "Legacy single-byte encodings"
          for e in g["encodings"]]

EOQ, CONTINUE, ERROR, FINISHED = object(), object(), object(), object()


def restore(queue, items):
    queue.extendleft(reversed(items))


def first_pointers(name, skip=lambda pointer: False):
    table = {}
    for pointer, cp in enumerate(index[name]):
        if cp is not None and not skip(pointer):
            table.setdefault(cp, pointer)
    return table


RANGES = index["gb18030-ranges"]
RANGE_POINTERS = [r[0] for r in RANGES]
RANGE_CODES = [r[1] for r in RANGES]


def ranges_code_point(pointer):
    if 39419 < pointer < 189000 or pointer > 1237575:
        return None
    if pointer == 7457:
        return 0xE7C7
    offset, code = RANGES[bisect_right(RANGE_POINTERS, pointer) - 1]
    return code + pointer - offset


def ranges_pointer(cp):
    if cp == 0xE7C7:
        return 7457
    offset, code = RANGES[bisect_right(RANGE_CODES, cp) - 1]
    return offset + cp - code


JIS0208 = index["jis0208"]
JIS0212 = index["jis0212"]
JIS0208_POINTER = first_pointers("jis0208")
SHIFT_JIS_POINTER = first_pointers("jis0208", lambda p: 8272 <= p <= 8835)
GB18030_POINTER = first_pointers("gb18030")
EUC_KR_POINTER = first_pointers("euc-kr")
BIG5_POINTER = first_pointers("big5", lambda p: p < (0xA1 - 0x81) * 157)
for pointer, cp in enumerate(index["big5"]):
    if cp in (0x2550, 0x255E, 0x2561, 0x256A, 0x5341, 0x5345) and pointer >= (0xA1 - 0x81) * 157:
        BIG5_POINTER[cp] = pointer
KATAKANA = index["iso-2022-jp-katakana"]
GB18030_SIDE = {0xE78D: b"\xA6\xD9", 0xE78E: b"\xA6\xDA", 0xE78F: b"\xA6\xDB", 0xE790: b"\xA6\xDC",
                0xE791: b"\xA6\xDD", 0xE792: b"\xA6\xDE", 0xE793: b"\xA6\xDF", 0xE794: b"\xA6\xEC",
                0xE795: b"\xA6\xED", 0xE796: b"\xA6\xF3", 0xE81E: b"\xFE\x59", 0xE826: b"\xFE\x61",
                0xE82B: b"\xFE\x66", 0xE82C: b"\xFE\x67", 0xE832: b"\xFE\x6D", 0xE843: b"\xFE\x7E",
                0xE854: b"\xFE\x90", 0xE864: b"\xFE\xA0"}


def index_at(table, pointer):
    return table[pointer] if pointer is not None and 0 <= pointer < len(table) else None


# Decoders: handler(queue, byte) gives a code point, a list of them, CONTINUE, ERROR or FINISHED.
class Utf8Decoder:
    def __init__(self):
        self.cp = self.seen = self.need = 0
        self.lower, self.upper = 0x80, 0xBF

    def handler(self, queue, byte):
        if byte is EOQ:
            if self.need:
                self.need = 0
                return ERROR
            return FINISHED
        if not self.need:
            if byte <= 0x7F:
                return byte
            if 0xC2 <= byte <= 0xDF:
                self.need, self.cp = 1, byte & 0x1F
            elif 0xE0 <= byte <= 0xEF:
                if byte == 0xE0:
                    self.lower = 0xA0
                if byte == 0xED:
                    self.upper = 0x9F
                self.need, self.cp = 2, byte & 0xF
            elif 0xF0 <= byte <= 0xF4:
                if byte == 0xF0:
                    self.lower = 0x90
                if byte == 0xF4:
                    self.upper = 0x8F
                self.need, self.cp = 3, byte & 0x7
            else:
                return ERROR
            return CONTINUE
        if not self.lower <= byte <= self.upper:
            self.cp = self.need = self.seen = 0
            self.lower, self.upper = 0x80, 0xBF
            restore(queue, [byte])
            return ERROR
        self.lower, self.upper = 0x80, 0xBF
        self.cp = self.cp << 6 | byte & 0x3F
        self.seen += 1
        if self.seen != self.need:
            return CONTINUE
        cp = self.cp
        self.cp = self.need = self.seen = 0
        return cp


class Utf16Decoder:
    def __init__(self, big):
        self.lead_byte = self.lead_surrogate = None
        self.big = big

    def handler(self, queue, byte):
        if byte is EOQ:
            if self.lead_byte is not None or self.lead_surrogate is not None:
                self.lead_byte = self.lead_surrogate = None
                return ERROR
            return FINISHED
        if self.lead_byte is None:
            self.lead_byte = byte
            return CONTINUE
        unit = (self.lead_byte << 8) + byte if self.big else (byte << 8) + self.lead_byte
        self.lead_byte = None
        if self.lead_surrogate is not None:
            lead, self.lead_surrogate = self.lead_surrogate, None
            if 0xDC00 <= unit <= 0xDFFF:
                return 0x10000 + (lead - 0xD800 << 10) + unit - 0xDC00
            b1, b2 = unit >> 8, unit & 0xFF
            restore(queue, [b1, b2] if self.big else [b2, b1])
            return ERROR
        if 0xD800 <= unit <= 0xDBFF:
            self.lead_surrogate = unit
            return CONTINUE
        if 0xDC00 <= unit <= 0xDFFF:
            return ERROR
        return unit


class SingleDecoder:
    def __init__(self, name):
        self.table = index[name.lower()] if name != "ISO-8859-8-I" else index["iso-8859-8"]

    def handler(self, queue, byte):
        if byte is EOQ:
            return FINISHED
        if byte < 0x80:
            return byte
        cp = self.table[byte - 0x80]
        return ERROR if cp is None else cp


class Gb18030Decoder:
    def __init__(self):
        self.first = self.second = self.third = 0

    def handler(self, queue, byte):
        if byte is EOQ:
            if self.first or self.second or self.third:
                self.first = self.second = self.third = 0
                return ERROR
            return FINISHED
        if self.third:
            if not 0x30 <= byte <= 0x39:
                restore(queue, [self.second, self.third, byte])
                self.first = self.second = self.third = 0
                return ERROR
            cp = ranges_code_point((self.first - 0x81) * 12600 + (self.second - 0x30) * 1260 +
                                   (self.third - 0x81) * 10 + byte - 0x30)
            self.first = self.second = self.third = 0
            return ERROR if cp is None else cp
        if self.second:
            if 0x81 <= byte <= 0xFE:
                self.third = byte
                return CONTINUE
            restore(queue, [self.second, byte])
            self.first = self.second = 0
            return ERROR
        if self.first:
            if 0x30 <= byte <= 0x39:
                self.second = byte
                return CONTINUE
            lead, self.first = self.first, 0
            offset = 0x40 if byte < 0x7F else 0x41
            pointer = (lead - 0x81) * 190 + byte - offset if 0x40 <= byte <= 0x7E or 0x80 <= byte <= 0xFE else None
            cp = index_at(index["gb18030"], pointer)
            if cp is not None:
                return cp
            if byte < 0x80:
                restore(queue, [byte])
            return ERROR
        if byte < 0x80:
            return byte
        if byte == 0x80:
            return 0x20AC
        if 0x81 <= byte <= 0xFE:
            self.first = byte
            return CONTINUE
        return ERROR


class Big5Decoder:
    def __init__(self):
        self.lead = 0

    def handler(self, queue, byte):
        if byte is EOQ:
            if self.lead:
                self.lead = 0
                return ERROR
            return FINISHED
        if self.lead:
            lead, self.lead = self.lead, 0
            offset = 0x40 if byte < 0x7F else 0x62
            pointer = (lead - 0x81) * 157 + byte - offset if 0x40 <= byte <= 0x7E or 0xA1 <= byte <= 0xFE else None
            pair = {1133: [0xCA, 0x304], 1135: [0xCA, 0x30C], 1164: [0xEA, 0x304], 1166: [0xEA, 0x30C]}
            if pointer in pair:
                return pair[pointer]
            cp = index_at(index["big5"], pointer)
            if cp is not None:
                return cp
            if byte < 0x80:
                restore(queue, [byte])
            return ERROR
        if byte < 0x80:
            return byte
        if 0x81 <= byte <= 0xFE:
            self.lead = byte
            return CONTINUE
        return ERROR


class EucJpDecoder:
    def __init__(self):
        self.jis0212 = False
        self.lead = 0

    def handler(self, queue, byte):
        if byte is EOQ:
            if self.lead:
                self.lead = 0
                return ERROR
            return FINISHED
        if self.lead == 0x8E and 0xA1 <= byte <= 0xDF:
            self.lead = 0
            return 0xFF61 - 0xA1 + byte
        if self.lead == 0x8F and 0xA1 <= byte <= 0xFE:
            self.jis0212 = True
            self.lead = byte
            return CONTINUE
        if self.lead:
            lead, self.lead = self.lead, 0
            cp = None
            if 0xA1 <= lead <= 0xFE and 0xA1 <= byte <= 0xFE:
                cp = index_at(JIS0212 if self.jis0212 else JIS0208, (lead - 0xA1) * 94 + byte - 0xA1)
            self.jis0212 = False
            if cp is not None:
                return cp
            if byte < 0x80:
                restore(queue, [byte])
            return ERROR
        if byte < 0x80:
            return byte
        if byte in (0x8E, 0x8F) or 0xA1 <= byte <= 0xFE:
            self.lead = byte
            return CONTINUE
        return ERROR


class Iso2022JpDecoder:
    def __init__(self):
        self.state = self.output_state = "ascii"
        self.lead = 0
        self.output = False

    def handler(self, queue, byte):
        state = self.state
        if state in ("ascii", "roman", "katakana", "lead"):
            if byte is EOQ:
                return FINISHED
            if byte == 0x1B:
                self.state = "escape start"
                return CONTINUE
            if state == "ascii" and byte <= 0x7F and byte not in (0x0E, 0x0F, 0x1B):
                self.output = False
                return byte
            if state == "roman" and byte <= 0x7F and byte not in (0x0E, 0x0F, 0x1B):
                self.output = False
                return {0x5C: 0xA5, 0x7E: 0x203E}.get(byte, byte)
            if state == "katakana" and 0x21 <= byte <= 0x5F:
                self.output = False
                return 0xFF61 - 0x21 + byte
            if state == "lead" and 0x21 <= byte <= 0x7E:
                self.output = False
                self.lead = byte
                self.state = "trail"
                return CONTINUE
            self.output = False
            return ERROR
        if state == "trail":
            if byte is EOQ:
                self.state = "lead"
                return ERROR
            if byte == 0x1B:
                self.state = "escape start"
                return ERROR
            self.state = "lead"
            if 0x21 <= byte <= 0x7E:
                cp = index_at(JIS0208, (self.lead - 0x21) * 94 + byte - 0x21)
                return ERROR if cp is None else cp
            return ERROR
        if state == "escape start":
            if byte in (0x24, 0x28):
                self.lead = byte
                self.state = "escape"
                return CONTINUE
            if byte is not EOQ:
                restore(queue, [byte])
            self.output = False
            self.state = self.output_state
            return ERROR
        lead, self.lead = self.lead, 0
        new = None
        if lead == 0x28 and byte == 0x42:
            new = "ascii"
        if lead == 0x28 and byte == 0x4A:
            new = "roman"
        if lead == 0x28 and byte == 0x49:
            new = "katakana"
        if lead == 0x24 and byte in (0x40, 0x42):
            new = "lead"
        if new is not None:
            self.state = self.output_state = new
            output, self.output = self.output, True
            return ERROR if output else CONTINUE
        restore(queue, [lead] if byte is EOQ else [lead, byte])
        self.output = False
        self.state = self.output_state
        return ERROR


class ShiftJisDecoder:
    def __init__(self):
        self.lead = 0

    def handler(self, queue, byte):
        if byte is EOQ:
            if self.lead:
                self.lead = 0
                return ERROR
            return FINISHED
        if self.lead:
            lead, self.lead = self.lead, 0
            offset = 0x40 if byte < 0x7F else 0x41
            lead_offset = 0x81 if lead < 0xA0 else 0xC1
            pointer = (lead - lead_offset) * 188 + byte - offset if 0x40 <= byte <= 0x7E or 0x80 <= byte <= 0xFC else None
            if pointer is not None and 8836 <= pointer <= 10715:
                return 0xE000 - 8836 + pointer
            cp = index_at(JIS0208, pointer)
            if cp is not None:
                return cp
            if byte < 0x80:
                restore(queue, [byte])
            return ERROR
        if byte <= 0x80:
            return byte
        if 0xA1 <= byte <= 0xDF:
            return 0xFF61 - 0xA1 + byte
        if 0x81 <= byte <= 0x9F or 0xE0 <= byte <= 0xFC:
            self.lead = byte
            return CONTINUE
        return ERROR


class EucKrDecoder:
    def __init__(self):
        self.lead = 0

    def handler(self, queue, byte):
        if byte is EOQ:
            if self.lead:
                self.lead = 0
                return ERROR
            return FINISHED
        if self.lead:
            lead, self.lead = self.lead, 0
            pointer = (lead - 0x81) * 190 + byte - 0x41 if 0x41 <= byte <= 0xFE else None
            cp = index_at(index["euc-kr"], pointer)
            if cp is not None:
                return cp
            if byte < 0x80:
                restore(queue, [byte])
            return ERROR
        if byte < 0x80:
            return byte
        if 0x81 <= byte <= 0xFE:
            self.lead = byte
            return CONTINUE
        return ERROR


class ReplacementDecoder:
    def __init__(self):
        self.returned = False

    def handler(self, queue, byte):
        if byte is EOQ:
            return FINISHED
        if not self.returned:
            self.returned = True
            return ERROR
        return FINISHED


class UserDefinedDecoder:
    def handler(self, queue, byte):
        if byte is EOQ:
            return FINISHED
        return byte if byte < 0x80 else 0xF780 + byte - 0x80


def decoder(name):
    return {"UTF-8": Utf8Decoder, "UTF-16BE": lambda: Utf16Decoder(True),
            "UTF-16LE": lambda: Utf16Decoder(False), "gb18030": Gb18030Decoder,
            "GBK": Gb18030Decoder, "Big5": Big5Decoder, "EUC-JP": EucJpDecoder,
            "ISO-2022-JP": Iso2022JpDecoder, "Shift_JIS": ShiftJisDecoder, "EUC-KR": EucKrDecoder,
            "replacement": ReplacementDecoder, "x-user-defined": UserDefinedDecoder
            }.get(name, lambda: SingleDecoder(name))()


def decode(name, data):
    """The decoder over data then end-of-queue: code points, None for each error."""
    instance, queue, output = decoder(name), deque(data), []
    while True:
        result = instance.handler(queue, queue.popleft() if queue else EOQ)
        if result is FINISHED:
            return output
        if result is ERROR:
            output.append(None)
        elif result is not CONTINUE:
            output.extend(result if isinstance(result, list) else [result])


# Encoders: handler(queue, code point) gives bytes, (ERROR, code point) or FINISHED.
class SimpleEncoder:
    def __init__(self, encode):
        self.encode = encode

    def handler(self, queue, cp):
        if cp is EOQ:
            return FINISHED
        if cp < 0x80:
            return bytes([cp])
        result = self.encode(cp)
        return (ERROR, cp) if result is None else result


def single_encoder(name):
    table = first_pointers(name.lower() if name != "ISO-8859-8-I" else "iso-8859-8")
    return lambda cp: bytes([table[cp] + 0x80]) if cp in table else None


def gb18030_encode(gbk):
    def encode(cp):
        if cp == 0xE5E5:
            return None
        if gbk and cp == 0x20AC:
            return b"\x80"
        if cp in GB18030_SIDE:
            return GB18030_SIDE[cp]
        pointer = GB18030_POINTER.get(cp)
        if pointer is not None:
            trail = pointer % 190
            return bytes([pointer // 190 + 0x81, trail + (0x40 if trail < 0x3F else 0x41)])
        if gbk:
            return None
        pointer = ranges_pointer(cp)
        return bytes([pointer // 12600 + 0x81, pointer % 12600 // 1260 + 0x30,
                      pointer % 1260 // 10 + 0x81, pointer % 10 + 0x30])
    return encode


def big5_encode(cp):
    pointer = BIG5_POINTER.get(cp)
    if pointer is None:
        return None
    trail = pointer % 157
    return bytes([pointer // 157 + 0x81, trail + (0x40 if trail < 0x3F else 0x62)])


def euc_jp_encode(cp):
    if cp == 0xA5:
        return b"\x5C"
    if cp == 0x203E:
        return b"\x7E"
    if 0xFF61 <= cp <= 0xFF9F:
        return bytes([0x8E, cp - 0xFF61 + 0xA1])
    pointer = JIS0208_POINTER.get(0xFF0D if cp == 0x2212 else cp)
    return None if pointer is None else bytes([pointer // 94 + 0xA1, pointer % 94 + 0xA1])


def shift_jis_encode(cp):
    if cp == 0x80:
        return b"\x80"
    if cp == 0xA5:
        return b"\x5C"
    if cp == 0x203E:
        return b"\x7E"
    if 0xFF61 <= cp <= 0xFF9F:
        return bytes([cp - 0xFF61 + 0xA1])
    pointer = SHIFT_JIS_POINTER.get(0xFF0D if cp == 0x2212 else cp)
    if pointer is None:
        return None
    lead, trail = pointer // 188, pointer % 188
    return bytes([lead + (0x81 if lead < 0x1F else 0xC1), trail + (0x40 if trail < 0x3F else 0x41)])


def euc_kr_encode(cp):
    pointer = EUC_KR_POINTER.get(cp)
    return None if pointer is None else bytes([pointer // 190 + 0x81, pointer % 190 + 0x41])


class Iso2022JpEncoder:
    def __init__(self):
        self.state = "ascii"

    def handler(self, queue, cp):
        if cp is EOQ:
            if self.state != "ascii":
                self.state = "ascii"
                return b"\x1B\x28\x42"
            return FINISHED
        if self.state in ("ascii", "roman") and cp in (0x0E, 0x0F, 0x1B):
            return ERROR, 0xFFFD
        if self.state == "ascii" and cp < 0x80:
            return bytes([cp])
        if self.state == "roman" and (cp < 0x80 and cp not in (0x5C, 0x7E) or cp in (0xA5, 0x203E)):
            return bytes([cp]) if cp < 0x80 else b"\x5C" if cp == 0xA5 else b"\x7E"
        if cp < 0x80:
            restore(queue, [cp])
            self.state = "ascii"
            return b"\x1B\x28\x42"
        if cp in (0xA5, 0x203E):
            restore(queue, [cp])
            self.state = "roman"
            return b"\x1B\x28\x4A"
        if cp == 0x2212:
            cp = 0xFF0D
        if 0xFF61 <= cp <= 0xFF9F:
            cp = KATAKANA[cp - 0xFF61]
        pointer = JIS0208_POINTER.get(cp)
        if pointer is None:
            if self.state == "jis0208":
                restore(queue, [cp])
                self.state = "ascii"
                return b"\x1B\x28\x42"
            return ERROR, cp
        if self.state != "jis0208":
            restore(queue, [cp])
            self.state = "jis0208"
            return b"\x1B\x24\x42"
        return bytes([pointer // 94 + 0x21, pointer % 94 + 0x21])


def output_encoding(name):
    return "UTF-8" if name in ("replacement", "UTF-16BE", "UTF-16LE") else name


def encoder(name):
    name = output_encoding(name)
    if name == "ISO-2022-JP":
        return Iso2022JpEncoder()
    return SimpleEncoder({
        "UTF-8": lambda cp: chr(cp).encode("utf-8"), "gb18030": gb18030_encode(False),
        "GBK": gb18030_encode(True), "Big5": big5_encode, "EUC-JP": euc_jp_encode,
        "Shift_JIS": shift_jis_encode, "EUC-KR": euc_kr_encode,
        "x-user-defined": lambda cp: bytes([cp - 0xF780 + 0x80]) if 0xF780 <= cp <= 0xF7FF else None,
    }.get(name) or single_encoder(name))


def encode_or_fail(instance, queue):
    """Bytes until an error or the end, and the error's code point (None at the end)."""
    output = b""
    while True:
        result = instance.handler(queue, queue.popleft() if queue else EOQ)
        if result is FINISHED:
            return output, None
        if isinstance(result, tuple):
            return output, result[1]
        output += result


def encode_html(name, text):
    instance, queue, output = encoder(name), deque(map(ord, text)), b""
    while True:
        result = instance.handler(queue, queue.popleft() if queue else EOQ)
        if result is FINISHED:
            return output
        if isinstance(result, tuple):
            restore(queue, [ord(c) for c in f"&#{result[1]};"])
        else:
            output += result


C0 = set(range(0x20))
QUERY = C0 | set(map(ord, ' "#<>'))
SPECIAL_QUERY = QUERY | {ord("'")}
FORM = set(range(0x80)) - set(map(ord, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789*-._"))


def percent_encode_after_encoding(name, text, encode_set, space_as_plus=False):
    instance, queue, output, error = encoder(name), deque(map(ord, text)), [], 0
    while error is not None:
        data, error = encode_or_fail(instance, queue)
        for byte in data:
            if space_as_plus and byte == 0x20:
                output.append("+")
            elif byte > 0x7E or byte in encode_set:
                output.append("%%%02X" % byte)
            else:
                output.append(chr(byte))
        if error is not None:
            output.append(f"%26%23{error}%3B")
    return "".join(output)


# Requests to the host test, and their expected results.
requests, expected = [], []


def ask(op, label, data, want, flags=0, chunk=0, what=""):
    label = label.encode("latin-1")
    requests.append(bytes([ord(op), len(label)]) + label + bytes([flags]) +
                    struct.pack("<II", chunk, len(data)) + data)
    expected.append((want, f"{op} {label.decode('latin-1')} [{chunk}/{flags}] {what}"))


def utf8(cps):
    return "".join(chr(0xFFFD if cp is None else cp) for cp in cps).encode("utf-8")


def fatal(cps):
    return (0, utf8(cps[:cps.index(None)])) if None in cps else (1, utf8(cps))


rng = random.Random(1028)

# Labels.
for label, name in LABELS.items():
    ask("L", "", label.encode(), (1, name.encode()), what=repr(label))
    ask("L", "", (" \t\n\f\r" + label.upper() + "\r\n ").encode(), (1, name.encode()), what=repr(label))
for name in NAMES:
    ask("L", "", name.encode(), (1, name.encode()), what=name)
for wrong in ("", " ", "utf-7", "utf-32", "unicode-1-1-utf-7", "x-mac-japanese", "\x0butf-8",
              "utf-8\x0b", "\u212aoi8-r", "utf-8\0", "utf8 ", "u\u0074f-8x", "iso-8859-1 1", "\u0130so-8859-2"):
    if wrong.strip(" \t\n\f\r").lower() not in LABELS or "\0" in wrong:
        ask("L", "", wrong.encode(), (0, b""), what=repr(wrong))

# Encoders: every BMP scalar value and a sample of the rest, one at a time.
scalars = [cp for cp in range(0x10000) if not 0xD800 <= cp <= 0xDFFF] + \
    list(range(0x10000, 0x110000, 61)) + [0x10FFFF, 0x1F600, 0x20000, 0x2A6D6, 0xE0100]
for name in NAMES:
    values = {}
    for cp in scalars:
        instance = encoder(name)
        data, error = encode_or_fail(instance, deque([cp]))
        data += encode_or_fail(instance, deque())[0]  # end-of-queue after an error
        values[cp] = bytes([len(data)]) + data + struct.pack("<I", 0xFFFFFFFF if error is None else error)
    for start in range(0, len(scalars), 16384):
        part = scalars[start:start + 16384]
        ask("S", name, struct.pack(f"<{len(part)}I", *part), ("S", part, values), what=f"scalars {start}")

# Decoders.
MULTI = ["UTF-8", "UTF-16BE", "UTF-16LE", "gb18030", "Big5", "EUC-JP", "ISO-2022-JP", "Shift_JIS", "EUC-KR"]
for name in NAMES:
    one = bytes(range(256))
    ask("D", name, one, (1, utf8(decode(name, one))), what="bytes 00-FF")
    ask("D", name, one, (1, utf8(decode(name, one))), chunk=1, what="bytes 00-FF, chunks of 1")
    for b in range(256):
        ask("D", name, bytes([b]), fatal(decode(name, bytes([b]))), flags=1, what=f"fatal {b:02X}")
for name in MULTI:
    for b0 in range(256):
        for b1 in range(256):
            data = bytes([b0, b1])
            cps = decode(name, data)
            ask("D", name, data, (1, utf8(cps)), chunk=1 + (b0 + b1) % 2, what=data.hex())
            if None in cps or (b0 + b1) % 7 == 0:
                ask("D", name, data, fatal(cps), flags=1, chunk=1 + b0 % 2, what="fatal " + data.hex())
for lead in (0x8E, 0x8F):
    for b1 in range(0xA1, 0xFF):
        data = b"".join(bytes([lead, b1, b2]) for b2 in range(0x80, 0x100))
        ask("D", "EUC-JP", data, (1, utf8(decode("EUC-JP", data))), chunk=1 + b1 % 3, what=data[:2].hex())
four, cps = bytearray(), []
for b1 in range(0x81, 0xFF):
    for b2 in range(0x30, 0x3A):
        for b3 in range(0x81, 0xFF):
            for b4 in range(0x30, 0x3A):
                four += bytes([b1, b2, b3, b4])
                cps.append(ranges_code_point((b1 - 0x81) * 12600 + (b2 - 0x30) * 1260 + (b3 - 0x81) * 10 + b4 - 0x30))
ask("D", "gb18030", bytes(four), (1, utf8(cps)), what="every four-byte sequence")
ask("D", "gb18030", bytes(four), (1, utf8(cps)), chunk=3, what="every four-byte sequence, chunks of 3")

POOLS = {
    "UTF-8": [*range(0x80), *range(0x80, 0xC2, 7), 0xC0, 0xC1, 0xC2, 0xDF, 0xE0, 0xE1, 0xEC, 0xED, 0xEE,
              0xEF, 0xF0, 0xF1, 0xF3, 0xF4, 0xF5, 0xFF, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xBB, 0xBF] * 2,
    "UTF-16BE": [0x00, 0x41, 0xD8, 0xDB, 0xDC, 0xDF, 0xFE, 0xFF, 0x30, 0x20],
    "gb18030": [0x41, 0x80, 0x81, 0x84, 0x90, 0xA3, 0xA6, 0xE3, 0xFE, 0xFF, 0x30, 0x35, 0x39, 0x40, 0x7E,
                0x7F, 0xA0, 0xD9],
    "Big5": [0x41, 0x80, 0x81, 0x87, 0x88, 0xA1, 0xA4, 0xC6, 0xF9, 0xFE, 0xFF, 0x40, 0x62, 0x64, 0x7E,
             0x7F, 0xA3, 0xA5, 0xA0],
    "EUC-JP": [0x41, 0x0A, 0x80, 0x8E, 0x8F, 0xA1, 0xA4, 0xB0, 0xDF, 0xE0, 0xFE, 0xFF, 0xA0],
    "ISO-2022-JP": [0x1B] * 6 + [0x24, 0x28] * 4 + [0x42, 0x4A, 0x49, 0x40] * 2 +
                   [0x21, 0x30, 0x41, 0x5C, 0x5F, 0x60, 0x7E, 0x7F, 0x0E, 0x0F, 0x0A, 0x0D, 0x80, 0xFF, 0x20],
    "Shift_JIS": [0x41, 0x5C, 0x7E, 0x80, 0x81, 0x87, 0x88, 0x9F, 0xA0, 0xA1, 0xDF, 0xE0, 0xEA, 0xED, 0xEE,
                  0xF0, 0xF9, 0xFA, 0xFC, 0xFD, 0xFF, 0x40, 0x7F],
    "EUC-KR": [0x41, 0x80, 0x81, 0xA1, 0xB0, 0xC8, 0xFE, 0xFF, 0x52, 0x5A, 0x61, 0x7A],
}
POOLS["UTF-16LE"] = POOLS["UTF-16BE"]
for name in MULTI + SINGLE[:3] + ["x-user-defined", "replacement", "GBK"]:
    pool = POOLS.get(name, list(range(256)))
    for i in range(400):
        data = bytes(rng.choice(pool) if rng.random() < 0.85 else rng.randrange(256)
                     for _ in range(rng.randrange(0, 48)))
        if name in ("UTF-8", "UTF-16BE", "UTF-16LE") and i % 5 == 0:
            data = {"UTF-8": b"\xEF\xBB\xBF", "UTF-16BE": b"\xFE\xFF", "UTF-16LE": b"\xFF\xFE"}[name] * (1 + i % 2) + data
        cps = decode(name, data)
        chunk = (1, 2, 3, 5, 4096)[i % 5]
        ask("D", name, data, (1, utf8(cps)), chunk=chunk, what=data.hex())
        ask("D", name, data, fatal(cps), flags=1, chunk=chunk, what="fatal " + data.hex())
        if name in ("UTF-8", "UTF-16BE", "UTF-16LE"):
            dropped = cps[1:] if cps[:1] == [0xFEFF] else cps
            ask("D", name, data, (1, utf8(dropped)), flags=2, chunk=chunk, what="BOM drop " + data.hex())
            ask("D", name, data, fatal(dropped), flags=3, chunk=chunk, what="fatal BOM drop " + data.hex())

    for i in range(12):  # past the decoder's output buffer
        data = bytes(rng.choice(pool) if rng.random() < 0.85 else rng.randrange(256)
                     for _ in range(rng.randrange(600, 5000)))
        cps = decode(name, data)
        for chunk in (1, 7, 4096):
            ask("D", name, data, (1, utf8(cps)), chunk=chunk, what=f"long {i}")
            ask("D", name, data, fatal(cps), flags=1, chunk=chunk, what=f"fatal long {i}")
# ISO-2022-JP's end-of-queue in each state.
for tail in ("1b", "1b24", "1b28", "1b2442", "1b2440", "1b284a", "1b2849", "1b244221", "1b24422121",
             "1b28491b24", "1b284a1b28", "1b2849211b", "1b2442211b24", "1b2442211b28", "1b24421b28"):
    for head in ("", "41", "1b284a5c", "1b284921", "1b24422121"):
        data = bytes.fromhex(head + tail)
        cps = decode("ISO-2022-JP", data)
        ask("D", "ISO-2022-JP", data, (1, utf8(cps)), chunk=1, what="end " + data.hex())
        ask("D", "ISO-2022-JP", data, (1, utf8(cps)), what="end " + data.hex())
        ask("D", "ISO-2022-JP", data, fatal(cps), flags=1, what="fatal end " + data.hex())

# WPT's ISO-2022-JP decoder cases, for the reference and the product alike.
wpt_cases = Path(r"D:\Programes\cloned repos\wpt\encoding\iso-2022-jp-decoder.any.js")
reference_failed = 0
for line in wpt_cases.read_text("utf-8").splitlines() if wpt_cases.exists() else []:
    case = re.match(r'decode\(\[([^\]]*)\], (".*?"), "', line)
    if case:
        data = bytes(int(b, 16) for b in case[1].split(","))
        want = ast.literal_eval(case[2]).encode("utf-8")
        reference_failed += utf8(decode("ISO-2022-JP", data)) != want
        ask("D", "ISO-2022-JP", data, (1, want), what="WPT " + data.hex())
wpt_cases = wpt_cases.with_name("iso-2022-jp-encoder.html")
for line in wpt_cases.read_text("utf-8").splitlines() if wpt_cases.exists() else []:
    case = re.match(r'\s*encode\((".*?"), (".*?"), "', line)
    if case:
        text, want = ast.literal_eval(case[1]), ast.literal_eval(case[2])
        reference_failed += percent_encode_after_encoding("ISO-2022-JP", text, SPECIAL_QUERY) != want
        ask("Q", "ISO-2022-JP", ("https://example.com/?" + text).encode(),
            (1, ("https://example.com/?" + want).encode()), what="WPT " + repr(text))
if reference_failed:
    sys.exit(f"the reference disagrees with WPT's ISO-2022-JP tests {reference_failed} times")

# decode: the BOM over the fallback.
for bom, name in ((b"\xEF\xBB\xBF", "UTF-8"), (b"\xFE\xFF", "UTF-16BE"), (b"\xFF\xFE", "UTF-16LE"), (b"", None)):
    for fallback in ("windows-1252", "Shift_JIS", "UTF-16LE", "UTF-8"):
        body = "a\u00e9\u3042".encode("utf-16-be" if name == "UTF-16BE" else "utf-16-le" if name == "UTF-16LE"
                                     else "utf-8") + b"\xFE\xFF"
        used = name or fallback
        ask("B", fallback, bom + body, (1, used.encode() + b"\0" + utf8(decode(used, body))), what=bom.hex())

# Strings: html mode, URL queries, urlencoded.
def pool_for(name):
    name = output_encoding(name)
    sample = [ord(c) for c in "aZ09 !\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"] + [0x0E, 0x0F, 0x1B, 0x7F]
    sample += [0x80, 0xA5, 0xE9, 0x203E, 0x2212, 0x20AC, 0xFF61, 0xFF9F, 0xE5E5, 0xE78D, 0xE7C7, 0x3000,
               0x2550, 0x5341, 0x1F600, 0x10FFFF, 0xF780, 0xF7FF, 0xFFFD, 0xFEFF]
    encodable = [cp for cp in rng.sample(range(0x80, 0x10000), 3000)
                 if not 0xD800 <= cp <= 0xDFFF and encode_or_fail(encoder(name), deque([cp]))[1] is None]
    return sample + encodable[:200] + [rng.randrange(0x80, 0xD800) for _ in range(40)]


SAFE = set(map(ord, "\t\n\r#"))
for name in NAMES:
    pool = pool_for(name)
    for i in range(300):
        text = "".join(chr(rng.choice(pool)) for _ in range(rng.randrange(0, 24)))
        ask("H", name, text.encode(), (1, encode_html(name, text)), what=repr(text))
        ask("P", name, text.encode(), (1, percent_encode_after_encoding(name, text, FORM, True).encode()),
            what=repr(text))
        query = "".join(c for c in text if ord(c) not in SAFE and ord(c) >= 0x20) + "x"
        for prefix, special in (("https://example.com/?", True), ("ws://example.com/?", True),
                                ("foo://h/?", False)):
            used = name if prefix.startswith("https") else "UTF-8"
            want = prefix + percent_encode_after_encoding(used, query, SPECIAL_QUERY if special else QUERY)
            ask("Q", name, (prefix + query).encode(), (1, want.encode()), what=repr(query))

work = out / "requests.bin"
work.write_bytes(b"".join(requests))
subprocess.run([str(exe), str(work), str(out / "results.bin")], check=True)
results = (out / "results.bin").read_bytes()

checks = failed = 0
at = 0
by_kind = {}


def fail(kind, message):
    global failed
    failed += 1
    by_kind.setdefault(kind, []).append(message)


for want, what in expected:
    status = struct.unpack_from("<b", results, at)[0]
    length = struct.unpack_from("<I", results, at + 1)[0]
    got = results[at + 5:at + 5 + length]
    at += 5 + length
    kind = " ".join(what.split()[:2])
    if want[0] == "S":  # value by value
        offset = 0
        for cp in want[1]:
            checks += 1
            n = got[offset] if offset < len(got) else 0
            value = got[offset:offset + 1 + n + 4]
            offset += 1 + n + 4
            if value != want[2][cp]:
                fail(kind, f"U+{cp:04X}: got {value.hex()} want {want[2][cp].hex()}")
        continue
    checks += 1
    if (status, got) != want:
        at_diff = next((i for i, (a, b) in enumerate(zip(got, want[1])) if a != b), min(len(got), len(want[1])))
        start = max(0, at_diff - 12)
        fail(kind, f"{what[:400]}: status {status}/{want[0]}, from byte {at_diff}: "
                   f"got {got[start:at_diff + 24]!r} want {want[1][start:at_diff + 24]!r}")

for kind, messages in by_kind.items():
    print(f"FAIL {kind}: {len(messages)}")
    for message in messages[:4]:
        print("   ", message)
print(f"encoding host check: {checks - failed}/{checks} passed")
sys.exit(1 if failed else 0)
