# Browser-side parser fuzzers (host, libFuzzer)

Host-only harnesses (never part of the guest image) for Nocturne-owned parsers that
see network or cross-process bytes. Each compiles the real product source into a
libFuzzer binary with ASan + UBSan; OS calls are stubbed in this directory
(`net_stub.c`, `shim/`, `host_compat.h`).

Run everything from the repo root in Git Bash with LLVM at
`C:\Program Files\LLVM`. Keep outputs and corpora outside the repo, e.g.
`OUT=$TEMP/fuzz-browser`.

    sh tests/fuzz/browser/build.sh <target> $OUT          # json cookie http url mse adaptive
    sh tests/fuzz/browser/build_image.sh $OUT             # image
    sh tests/fuzz/browser/build_font_vendor.sh $OUT       # FreeType + brotli objects, once
    sh tests/fuzz/browser/build_font.sh $OUT              # font
    cd $OUT && ./fuzz_<target>.exe corpus_<target> -max_total_time=180 -max_len=4096

Targets

| target   | product source                                   | input                                       |
|----------|--------------------------------------------------|---------------------------------------------|
| json     | `user/libc/json.c`                               | JSON text, parse + serialize round trip     |
| cookie   | `user/libc/webcookie.c`                          | Set-Cookie value, jar snapshot import/export|
| http     | `user/libc/http.c`, `http2.c`, gzip/inflate      | server bytes: HTTP/1.1 (chunked, gzip) or h2 frames + HPACK (byte 0: bit0=h2, bit1=collect body, bits 2-7 read-chunk size) |
| url      | `user/libc/http.c` URL parsers                   | URL strings                                 |
| mse      | `user/libc/media_mse_parser.c`                   | MP4/WebM framing (byte 0: bit0 webm, bit1 have_init, bit2 eos) |
| adaptive | `user/libc/media_adaptive_parser.c`, `media_http.c` URI resolve | HLS / DASH manifests (byte 0 bit0 = DASH) |
| image    | `user/libc/image.c` + `third_party_img.c`        | ICO/PNG/JPEG/GIF/BMP/WebP/SVG (last 4 bytes pick scale/draw args) |
| font     | `user/libc/font.c`, `font_sfnt.h`, FreeType      | TTF/WOFF/WOFF2/OTF/EOT, then measure + rasterize text |

Seed corpora: any small valid sample of the format (`.ttf`/`.woff2` for font, an ICO
with a DIB image, an HLS/MPD manifest, a fragmented-MP4 `moof`...). The harnesses
discover the rest.

Notes

- The Windows SDK ships its own `<http.h>`; `shim/http.h` forces the Nocturne one.
- The host `long` is 32-bit, so LLP64-only findings are not guest bugs; fix them
  only when they are also wrong in principle.
- The wasm module image reader (`js_wasm.cpp`) and the worker wire are not covered
  here (separate owners / tiny fixed headers).
