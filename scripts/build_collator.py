"""Export pinned ICU collation elements; ICU is a HOST generation tool only.

The guest consumes the checked-in packed tables, never this library. Public
ICU element/set APIs preserve expansions, contractions and prefix contexts.
Use ICU 78.1 (Unicode 17 / CLDR 48), not the host's default locale database.
"""
import argparse
import base64
import bisect
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'third_party/unicode_collation'


class ICU:
    def __init__(self, directory):
        directory = Path(directory).resolve()
        if os.name == 'nt':
            self.directory_handle = os.add_dll_directory(str(directory))
            self.uc = C.CDLL(str(directory / 'icuuc78.dll'))
            self.i18n = C.CDLL(str(directory / 'icuin78.dll'))
        else:
            self.uc = C.CDLL(str(directory / 'libicuuc.so.78'))
            self.i18n = C.CDLL(str(directory / 'libicui18n.so.78'))
        self.files = {}
        libraries = [Path(lib._name) for lib in (self.uc, self.i18n)]
        data_file = directory / 'icudt78.dll'
        if data_file.exists():
            libraries.append(data_file)
        for p in libraries:
            self.files[p.name] = hashlib.sha256(p.read_bytes()).hexdigest()
        def fn(name, result, *args):
            lib = self.i18n if name.startswith('ucol_') else self.uc
            f = getattr(lib, name + '_78')
            f.restype, f.argtypes = result, list(args)
            setattr(self, name, f)
        P, I, U, B = C.c_void_p, C.c_int32, C.c_uint32, C.c_int8
        EP, UP = C.POINTER(I), C.POINTER(C.c_uint16)
        fn('u_getVersion', None, C.POINTER(C.c_uint8))
        fn('u_getUnicodeVersion', None, C.POINTER(C.c_uint8))
        fn('u_getCombiningClass', C.c_uint8, I)
        fn('u_charDigitValue', I, I)
        fn('unorm2_getNFDInstance', P, EP)
        fn('unorm2_normalize', I, P, UP, I, UP, I, EP)
        fn('ucol_open', P, C.c_char_p, EP)
        fn('ucol_close', None, P)
        fn('ucol_setAttribute', None, P, I, I, EP)
        fn('ucol_getVariableTop', U, P, EP)
        fn('ucol_getContractionsAndExpansions', None, P, P, P, B, EP)
        fn('ucol_openElements', P, P, UP, I, EP)
        fn('ucol_setText', None, P, UP, I, EP)
        fn('ucol_next', I, P, EP)
        fn('ucol_closeElements', None, P)
        fn('ucol_getSortKey', I, P, UP, I, C.POINTER(C.c_uint8), I)
        fn('ucol_strcoll', I, P, UP, I, UP, I)
        fn('uset_openEmpty', P)
        fn('uset_close', None, P)
        fn('uset_getItemCount', I, P)
        fn('uset_getItem', I, P, I, C.POINTER(I), C.POINTER(I), UP, I, EP)
        version = (C.c_uint8 * 4)()
        self.u_getVersion(version)
        self.version = list(version)
        if self.version[:2] != [78, 1]:
            raise RuntimeError('ICU 78.1 required: ' + str(self.version))
        self.u_getUnicodeVersion(version)
        self.unicode = list(version)
        error = I()
        self.nfd = self.unorm2_getNFDInstance(C.byref(error))
        self.checked(error)

    @staticmethod
    def text(s):
        raw = s.encode('utf-16-le', 'surrogatepass')
        return (C.c_uint16 * (len(raw) // 2)).from_buffer_copy(raw)

    @staticmethod
    def checked(error):
        if error.value > 0:
            raise RuntimeError('ICU error ' + str(error.value))

    def elements(self, iterator, s):
        text, error = self.text(s), C.c_int32()
        self.ucol_setText(iterator, text, len(text), C.byref(error))
        result = []
        while True:
            ce = self.ucol_next(iterator, C.byref(error))
            if ce == -1:
                break
            ce &= 0xffffffff
            # Reconstitute ICU's 64-bit CE from the public iterator's halves.
            # coleitr.cpp getFirstHalf/getSecondHalf; continuation marker 0xc0.
            if ce & 0xc0 == 0xc0:
                p, s, t = result[-1]
                result[-1] = (p | (ce >> 16), s | ((ce >> 8) & 255),
                              t | (ce & 0x3f))
            else:
                result.append((ce & 0xffff0000, (ce >> 8 & 255) << 8,
                               (ce & 255) << 8))
        self.checked(error)
        return tuple(result)

    def strings(self, collator):
        contractions, error = self.uset_openEmpty(), C.c_int32()
        self.ucol_getContractionsAndExpansions(collator, contractions, None,
                                              1, C.byref(error))
        self.checked(error)
        result = set()
        for index in range(self.uset_getItemCount(contractions)):
            start, end = C.c_int32(), C.c_int32()
            buf = (C.c_uint16 * 128)()
            length = self.uset_getItem(contractions, index, C.byref(start),
                                      C.byref(end), buf, len(buf), C.byref(error))
            self.checked(error)
            if length > 0:
                result.add(bytes(buf)[:length * 2].decode('utf-16-le', 'surrogatepass'))
        self.uset_close(contractions)
        return result

    def normalize(self, s):
        text, error = self.text(s), C.c_int32()
        out = (C.c_uint16 * 512)()
        n = self.unorm2_normalize(self.nfd, text, len(text), out, len(out), C.byref(error))
        self.checked(error)
        return bytes(out)[:n*2].decode('utf-16-le', 'surrogatepass')


def packed(values):
    raw = bytearray()
    for value in values:
        value &= 0xffffffff
        while value >= 128:
            raw.append((value & 127) | 128)
            value >>= 7
        raw.append(value)
    return base64.b64encode(raw).decode('ascii')


def table(icu, locale, stats, base=None):
    error = C.c_int32()
    collator = icu.ucol_open(locale.encode('ascii'), C.byref(error))
    icu.checked(error)
    # Explicit normalization and non-ignorable tables. Sensitivity, caseFirst
    # and shifted punctuation are guest comparison parameters, not new tables.
    icu.ucol_setAttribute(collator, 4, 17, C.byref(error))  # normalization on
    icu.ucol_setAttribute(collator, 1, 21, C.byref(error))  # non-ignorable
    icu.ucol_setAttribute(collator, 5, 0, C.byref(error))   # primary strength
    icu.checked(error)
    variable_top = icu.ucol_getVariableTop(collator, C.byref(error))
    iterator = icu.ucol_openElements(collator, None, 0, C.byref(error))
    icu.checked(error)
    pool, pool_ids = [], {}
    ranges, pending = [], None
    base_starts = base and base['_ranges'][::6]
    def base_ces(cp):
        r = base['_ranges']
        index = (bisect.bisect_right(base_starts, cp)-1)*6
        if r[index+3] == 0xffffffff:
            return base['_pool'][r[index+2]]
        return (((r[index+2] + r[index+3]*(cp-r[index])) & 0xffffffff,
                 r[index+4], r[index+5]),)
    # Pinned ICU's primary reorder is exported as an order-preserving mapping.
    # For single-CE strings the uncompressed primary key bytes are the primary
    # weight (ICU CollationKeys, pinned version). No generic key parser is used.
    reorder = {}
    key = (C.c_uint8 * 32)()
    def export(ces, text, store=True):
        for p, _s, _t in ces:
            if p and p >> 16 not in reorder:
                if len(ces) == 1:
                    buf = icu.text(text)
                    size = icu.ucol_getSortKey(collator, buf, len(buf), key, len(key))
                    if size <= 5:
                        primary = int.from_bytes(bytes(key)[:size-1].ljust(4, b'\0'), 'big')
                        reorder[p >> 16] = (primary >> 16) - (p >> 16)
        if not store:
            return None
        if ces not in pool_ids:
            pool_ids[ces] = len(pool)
            pool.append(ces)
        return pool_ids[ces]
    for cp in range(0x110000):
        text = chr(cp)
        ces = icu.elements(iterator, text)
        export(ces, text, False)
        if base and ces == base_ces(cp):
            if pending:
                ranges.extend(pending)
                pending = None
            continue
        # A compact affine range needs no pool entry; this also retains actual
        # ICU implicit weights for non-BMP, private-use and unassigned scalars.
        if len(ces) == 1:
            p, s, t = ces[0]
            if pending and pending[1] + 1 == cp and pending[4:] == [s, t] and (
                    (cp == pending[0] + 1) or p == pending[2] + pending[3] * (cp-pending[0])):
                if cp == pending[0] + 1:
                    pending[3] = p - pending[2]
                pending[1] = cp
                continue
            if pending:
                ranges.extend(pending)
            pending = [cp, cp, p, 0, s, t]
        else:
            if pending:
                ranges.extend(pending)
                pending = None
            ranges.extend([cp, cp, export(ces, text), 0xffffffff, 0, 0])
    if pending:
        ranges.extend(pending)
    strings, contexts = [], []
    for text in sorted({icu.normalize(s) for s in icu.strings(collator)}):
        # Runtime input is NFD. ICU supplies canonically closed contraction sets;
        # keeping all entries retains prefixes and contextual Japanese kana.
        strings.append([text, export(icu.elements(iterator, text), text)])
    for text, index in strings:
        contexts.extend([len(text), *(ord(ch) for ch in text), index])
    # Fill sparse high-16 mapping holes from the same lead-byte range. Japanese
    # split lead-byte reordering is represented by explicit high-16 boundaries.
    order = sorted(reorder.items())
    boundaries = []
    last = None
    for high, delta in order:
        if delta != last:
            boundaries.extend([high, delta & 0xffffffff])
            last = delta
    flat_pool, offsets = [], [0]
    for ces in pool:
        for p, s, t in ces:
            flat_pool.extend([p, s, t])
        offsets.append(len(flat_pool))
    icu.ucol_closeElements(iterator)
    icu.ucol_close(collator)
    stats[locale] = {'ranges': len(ranges)//6, 'pool':len(pool),
                     'contexts':len(strings), 'reorderRanges':len(boundaries)//2}
    return {'ranges':packed(ranges), 'pool':packed(flat_pool),
            'offsets':packed(offsets), 'contexts':packed(contexts),
            'reorder':boundaries, 'variableTop':variable_top,
            '_ranges':ranges, '_pool':pool}


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--icu-dir')
    parser.add_argument('--emit-only', action='store_true', help='Re-embed the existing pinned data; no ICU required')
    args = parser.parse_args()
    if args.emit_only:
        emit()
        return
    if not args.icu_dir:
        parser.error('--icu-dir is required unless --emit-only is specified')
    icu = ICU(args.icu_dir)
    VENDOR.mkdir(parents=True, exist_ok=True)
    stats, tables = {}, {}
    base = None
    for name, locale in [('enSort','en'),('enSearch','en@collation=search'),
                         ('jaSort','ja')]:
        print('Extracting', locale, flush=True)
        tables[name] = table(icu, locale, stats, base)
        if base is None:
            base = tables[name]
    # Japanese search inherits the CLDR root search data unchanged. Verify the
    # complete table identity during generation before sharing its storage.
    print('Verifying Japanese search data identity', flush=True)
    ja_search = table(icu, 'ja@collation=search', stats, base)
    if ja_search != tables['enSearch']:
        tables['jaSearch'] = ja_search
    else:
        tables['jaSearch'] = 'enSearch'
    for value in tables.values():
        if isinstance(value, dict):
            value.pop('_ranges')
            value.pop('_pool')
    ccc, digits = [], []
    for cp in range(0x110000):
        cc = icu.u_getCombiningClass(cp)
        if cc:
            ccc.extend([cp, cc])
        digit = icu.u_charDigitValue(cp)
        if digit == 0:
            digits.append(cp)
    data = {'tables':tables, 'ccc':packed(ccc), 'digitStarts':digits}
    target = VENDOR / 'data.json'
    target.write_text(json.dumps(data, ensure_ascii=True, separators=(',',':'))+'\n',
                      encoding='utf-8', newline='\n')
    manifest = {'icu':icu.version,'unicode':icu.unicode,'cldr':'48',
                'upstream':'https://github.com/unicode-org/icu/tree/release-78.1',
                'librarySha256':icu.files,'dataSha256':hashlib.sha256(target.read_bytes()).hexdigest(),
                'tables':stats,'locales':['en','ja']}
    (VENDOR / 'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    emit()


def emit():
    source = (VENDOR / 'runtime.js').read_text(encoding='utf-8')
    data = (VENDOR / 'data.json').read_text(encoding='utf-8').strip()
    source = source.replace('/* @collation_data */ null', data)
    (ROOT / 'user/libc/web/js_collator.js').write_text(source, encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
