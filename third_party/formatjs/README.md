# Nocturne Intl

Fixed FormatJS package versions and SHA-256 hashes are in `manifest.json`;
`package-lock.json` records npm integrity and provenance. Only the 50 source and
license files used by the bundle are vendored, not node_modules or the full
multi-language data packages. Each package retains its MIT `LICENSE.md`.
Unicode/CLDR and IANA notices are retained alongside them.

The browser uses the generated `user/libc/web/js_intl.js`. It does not use Node,
host locale facilities, a POSIX layer, or any new Nocturne system call.
To regenerate, provide the host esbuild 0.28.2 executable to
`python scripts/build_intl.py --esbuild /path/to/esbuild`; `--check` compares
the result with the checked-in bundle instead of writing it. The script checks
the vendored files against `manifest.json`, copies them to `build/nocturne-intl`
and applies `patches/*.patch` there in name order, so the vendored files stay as
published and each Nocturne change is a reviewable diff with its reason.

`patches/datetimeformat-lazy-patterns.patch` defers parsing a format's pattern,
12-hour pattern and range patterns until one of them is read. The first
DateTimeFormat of a realm otherwise parses every format of its calendar
(760 English Gregorian formats with 2,995 range patterns) although the format
matchers read only skeleton fields; the patch header records the comparison.

The bundle provides getCanonicalLocales, Locale, PluralRules, NumberFormat,
DateTimeFormat, RelativeTimeFormat, ListFormat and DisplayNames, with English and
Japanese data. Other locale requests use the upstream specified fallback;
supportedLocalesOf does not advertise unshipped language data. Collator,
Segmenter is provided separately by `../formatjs-segmenter/`. DurationFormat
is not implemented. Locale.getCollations depends
on the unimplemented Collator and is consequently not a complete supported path.
DateTimeFormat inherits upstream Gregorian/ISO calendar support and a timezone
transition table extending through 2100. The default zone is UTC; Nocturne has
no browser timezone-preference host contract in this integration.

`lazy-tz.js` retains all upstream packed IANA zone tables, but invokes the original
decoder per-zone at first use instead of eagerly expanding all tables. It does
not change the decoded data. Auxiliary comparison against the eager upstream
bundle checked 452 tables, 89,268 transitions and 2,260 formatted values with no
difference. This reduces startup heap from approximately 23.5 to 8.0 MB in the
CONFIG_NOCTURNE QuickJS harness; first use of DateTimeFormat still allocates
locale skeleton data. Browser/QEMU and real-site acceptance are separate tests.

Upstream: https://github.com/formatjs/formatjs
