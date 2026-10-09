# Nocturne Intl.Segmenter

Pinned FormatJS `@formatjs/intl-segmenter` 12.2.15 and
`@formatjs/intl-localematcher` 0.9.0, obtained from their official npm tarballs.
The existing `../formatjs/source/@formatjs/fast-memoize` 3.1.7 is reused.
`manifest.json` records source SHA-256 digests; every package retains its MIT
license. Unicode/CLDR notice: `../formatjs/LICENSE-UNICODE.txt`.

The generated browser module is `user/libc/web/js_segmenter.js`, loaded after
the existing Intl module. It preserves an existing native Segmenter. To
regenerate offline, run `python scripts/build_segmenter.py --esbuild PATH`
with esbuild 0.28.2. No npm, network, host Intl, POSIX runtime, or site-specific
hook is required by the browser or normal Nocturne build.

The upstream implementation provides grapheme, word and sentence segmentation,
UTF-16 indices, reusable iterable Segments, independent iterators, containing,
option validation and locale matching. It includes Unicode/CLDR boundary
rules and locale tailoring, not a dictionary segmentation engine. In particular,
Japanese/Thai word boundaries are not claimed to match native ICU dictionaries.
Locale selection does not establish complete dictionary segmentation coverage.

Upstream: https://github.com/formatjs/formatjs
API documentation: https://formatjs.github.io/docs/polyfills/intl-segmenter/
