"""Host check of user/libc/http_coding.h, the content-coding decoders of an HTTP response.

Builds tests/http_coding_hosttest.c with the product DEFLATE codec (deflate.c) and brotli
adapter and library, then feeds it
WPT's fetch/content-encoding resources (br, gzip; zstd is not supported, so its bytes must come
back untouched) and deflate, layered and broken bodies made here, cut at several read sizes.
Not live websites and not the guest: http.c's header parsing and the guest run are outside it.
"""
from pathlib import Path
import gzip
import subprocess
import sys
import zlib

root = Path(__file__).resolve().parent.parent
wpt = Path(r"D:\Programes\cloned repos\wpt\fetch\content-encoding")
out = root / "build" / "coding-host"
out.mkdir(parents=True, exist_ok=True)
exe = out / "http_coding_hosttest.exe"
brotli = [str(p) for part in ("common", "dec", "enc") for p in sorted((root / "third_party/brotli/c" / part).glob("*.c"))]
subprocess.run([str(root / "tools/msys64/ucrt64/bin/clang.exe"), "-O2", "-DBROTLI_BUILD_PORTABLE",
                "-I" + str(root / "third_party/brotli/c/include"), "-idirafter", str(root / "user/include"),
                str(root / "tests/http_coding_hosttest.c"), str(root / "user/libc/deflate.c"),
                str(root / "user/libc/third_party_brotli.c"), *brotli, "-o", str(exe)], check=True)

checks = failed = 0
CHUNKS = (1, 2, 3, 7, 64, 4096)


def run(coding, data, chunk):
    p = subprocess.run([str(exe), coding, str(chunk)], input=data, capture_output=True)
    return p.returncode, p.stdout, p.stderr.decode("utf-8", "replace").strip()


def expect(name, coding, data, want=None, error=None):
    """want: the decoded bytes; error: the start of the decoder's message when it must fail."""
    global checks, failed
    for chunk in CHUNKS:
        checks += 1
        code, got, message = run(coding, data, chunk)
        good = (code == 0 and got == want) if error is None else (code == 1 and message.startswith(error))
        if not good:
            failed += 1
            print(f"FAIL {name} [{coding!r}, chunk {chunk}]: exit {code}, {len(got)} bytes, {message!r}")


def read(path):
    return path.read_bytes()


# WPT resources: br and gzip of the same texts.
for stem in ("foo.text", "foo.octetstream", "big.text", "hello.html"):
    plain = gzip.decompress(read(wpt / "gzip/resources" / (stem + ".gz")))
    expect("wpt gzip " + stem, "gzip", read(wpt / "gzip/resources" / (stem + ".gz")), plain)
    expect("wpt br " + stem, "br", read(wpt / "br/resources" / (stem + ".br")), plain)
    if stem.startswith("foo."):
        checks += 1
        if len(plain) != 10500:  # br-body.https.any.js: expectedDecompressedSize
            failed += 1
            print("FAIL wpt size", stem, len(plain))
    # zstd is not a coding this decoder has: Fetch hands the bytes on as they are.
    zst = read(wpt / "zstd/resources" / (stem + ".zst"))
    expect("wpt zstd passes " + stem, "zstd", zst, zst)

text = read(wpt / "gzip/resources/big.text.gz")
text = gzip.decompress(text)
raw = zlib.compressobj(9, zlib.DEFLATED, -15)
raw = raw.compress(text) + raw.flush()
wrapped = zlib.compress(text, 9)

expect("x-gzip", "X-GZip", gzip.compress(text), text)
expect("gzip members", "gzip", gzip.compress(text[:5000]) + gzip.compress(text[5000:]), text)
expect("deflate zlib", "deflate", wrapped, text)
expect("deflate bare", "Deflate", raw, text)
expect("deflate stored", "deflate", zlib.compress(text, 0), text)
expect("empty deflate", "deflate", zlib.compress(b""), b"")
expect("identity only", "identity", b"as it is", b"as it is")
expect("empty elements", " , gzip ,, identity ,", gzip.compress(text), text)
# Layered: the list is in the order the server applied them; the last is undone first.
expect("deflate then gzip", "deflate, gzip", gzip.compress(wrapped), text)
expect("br then gzip", "br, gzip", gzip.compress(read(wpt / "br/resources/big.text.br")), text)
expect("gzip then deflate", "gzip,deflate", zlib.compress(gzip.compress(text)), text)
# Not supported or not a list of tokens: Fetch's "handle content codings" returns the bytes.
for coding in ("gzip, zstd", "compress", "gz ip", "gzip;q=1", "br\x7f"):
    expect("passes " + repr(coding), coding, b"\x00coded?", b"\x00coded?")

# Broken bodies fail (WPT bad-*-body: the body read rejects).
expect("wpt bad br", "br", b"not actually br", error="invalid or truncated br")
expect("wpt bad gzip", "gzip", b"not actually gzip", error="invalid or truncated gzip")
expect("truncated gzip", "gzip", gzip.compress(text)[:-1], error="invalid or truncated gzip")
expect("truncated br", "br", read(wpt / "br/resources/big.text.br")[:-1], error="invalid or truncated br")
expect("br trailing", "br", read(wpt / "br/resources/foo.text.br") + b"x", error="invalid or truncated br")
expect("truncated zlib", "deflate", wrapped[:-1], error="invalid or truncated deflate")
bad_adler = wrapped[:-1] + bytes([wrapped[-1] ^ 1])
expect("zlib checksum", "deflate", bad_adler, error="invalid or truncated deflate")
expect("zlib trailing", "deflate", wrapped + b"x", error="invalid or truncated deflate")
expect("bare trailing", "deflate", raw + b"xx", error="invalid or truncated deflate")
expect("truncated bare", "deflate", raw[:-1], error="invalid or truncated deflate")
# No bytes at all (a redirect with the field and an empty body) is an empty body.
expect("empty gzip", "gzip", b"", b"")
expect("empty br", "br", b"", b"")
expect("empty layers", "deflate, gzip", b"", b"")
expect("inner layer broken", "deflate, gzip", gzip.compress(bad_adler), error="invalid or truncated deflate")

print(f"http coding: {checks} checks, {failed} failed")
sys.exit(1 if failed else 0)
