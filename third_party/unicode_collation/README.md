# Nocturne Unicode collation

`user/libc/web/js_collator.js` adds `Intl.Collator` and the ECMA-402
`String.prototype.localeCompare` path after the existing FormatJS Intl bundle.
It is an independent guest JavaScript implementation, **not** a call to Node,
Windows/host locale APIs, ICU DLLs, or POSIX services. QuickJS's shipped Unicode
17 `String.normalize("NFD")` supplies canonical normalization.

## Fixed upstream data and attribution

The checked-in tables are exported from official ICU **78.1**, Unicode **17.0**,
CLDR **48**. `upstream.json` records the official release URL, archive SHA-256
and upstream SHA-512 checksum source. `manifest.json` records the three host
library SHA-256 hashes, generated-data SHA-256, and per-table counts. `LICENSE`
is the complete upstream ICU license/notices (Unicode-3.0 and historic notices).
The numeric CE encoder and CE comparison bit masks follow the pinned ICU source
algorithms and retain that attribution here and in `runtime.js`:

- [ICU78.1 collationiterator.cpp](https://github.com/unicode-org/icu/blob/release-78.1/icu4c/source/i18n/collationiterator.cpp)
- [ICU78.1 collationcompare.cpp](https://github.com/unicode-org/icu/blob/release-78.1/icu4c/source/i18n/collationcompare.cpp)
- [ICU78.1 coleitr.cpp](https://github.com/unicode-org/icu/blob/release-78.1/icu4c/source/i18n/coleitr.cpp)
- [ICU78.1 collationkeys.cpp](https://github.com/unicode-org/icu/blob/release-78.1/icu4c/source/i18n/collationkeys.cpp)
- [Unicode Collation Algorithm](https://www.unicode.org/reports/tr10/)
- [ECMA-402 Collator](https://tc39.es/ecma402/#collator-objects)

Host generation enumerates all code points through `ucol_openElements` /
`ucol_next`, reconstructing ICU's complete primary/secondary/tertiary CEs from
the public iterator's continuation halves. It exports all contractions and
prefix contexts from `ucol_getContractionsAndExpansions(addPrefixes=true)` with
canonical closure, including Japanese prolonged-vowel and iteration contexts.
This includes non-BMP, private-use, unassigned and isolated-surrogate implicit
weights; unknown code points do not fall back to ASCII/uppercase comparison.
For primary script reordering, the exporter extracts the fixed ICU version's
single-CE primary key bytes and preserves the high-16-bit offset boundaries.
This is a **version-specific** data adapter, not a promise that arbitrary ICU
versions share a sort-key encoding. Upgrading ICU requires regenerating and
validating all data and oracle vectors together.

## Runtime and honest scope

Only **English and Japanese** locale data are advertised by `supportedLocalesOf`.
Other requested locales fall back to English. Only the locale-default sort and
search collations are shipped: unsupported `co` requests resolve to `default`,
not to a falsely advertised `emoji`, `eor`, `unihan`, or `phonebk` collation.
`base`, `accent`, `case`, and `variant` sensitivities, Unicode decimal-digit
numeric collation (including ICU's 254-digit segment boundaries), `caseFirst`,
and shifted punctuation are comparison parameters, not ASCII approximations.
The ECMA-402 getter caches an unconstructable bound compare function; WeakMap
slots enforce branding and options follow the specified observable read order.

The packed tables use unsigned LEB128/base64 strings, decoded into TypedArrays
only on first comparison. English sort is the common base; search and Japanese
sort store differences. Generation verifies the complete Japanese search table
equals English search before aliasing them. Context tries are likewise built
only for tables used by a comparison. No table is decoded by merely constructing
a Collator or querying supported/resolved locales.

`js_collator_native.c/.h` supplies the browser-private CPU-only packed decoder:
register `web_js_collator_init(ctx, host)` before bootstrap. The captured helper
decodes directly into a QuickJS-accounted `js_malloc` buffer and transfers its
ownership to an ArrayBuffer with a `js_free_rt` finalizer. The JavaScript engine
creates a Uint32Array view without copying. Strict base64/uint32-LEB validation,
a 4 MiB per-table input cap and checked construction failure paths bound all
native work. There is no ICU/native locale call, OS syscall, POSIX layer,
watchdog extension, prewarming, or user-data dependency. This removes millions
of interpreted decode operations from the first page comparison. Context tries
remain lazy and linear in the small context tables, not in the Unicode range.
The JavaScript decoder is retained for isolated portable host validation where
no browser `host` binding exists. `tests/js_collator_native.c` separately checks
numeric boundaries, strict malformed-data rejection and the private size cap.

## Reproduction (explicit host operation)

The optional download is about 29.4 MB and is never part of the OS build:

```text
python -X utf8 scripts/fetch_collator_icu.py
python -X utf8 scripts/build_collator.py --icu-dir build/collator-icu78-host
python -X utf8 scripts/generate_collator_oracle.py --icu-dir build/collator-icu78-host
```

The fetcher verifies the official archive checksum, extracts only the three
generator DLLs into the ignored host work directory, and retains notices.
For source-only edits without ICU or any network:

```text
python -X utf8 scripts/build_collator.py --emit-only
```

`test_cases.js` is the authored test source and `tests/js_collator_cases.js` is
its generated browser-batch version. The generator records upstream expected
comparison signs; it does **not** execute the guest or prove its correctness.
`runCollatorCases()` checks the ECMA-402 surface and an ICU option matrix,
canonical equivalence, expansions, Japanese contexts, script order, punctuation,
numeric boundaries, non-BMP/implicit values, and deterministic mixed-string
vectors. It yields between groups of checks for the incremental browser host.
Run it together with the existing Intl, Shadow DOM and browser suites; successful
host extraction/static syntax checks do not establish QEMU or real-site success.
