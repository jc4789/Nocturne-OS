"""Prepare comparison vectors with pinned upstream ICU; does not run the guest.

The combined browser batch executes tests/js_collator_cases.js afterwards.
"""
import argparse
import ctypes as C
import itertools
import json
import random
from pathlib import Path
from build_collator import ICU, ROOT, VENDOR


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument('--icu-dir', required=True)
    args = parser.parse_args()
    icu = ICU(args.icu_dir)
    groups = []
    pairs = [('A','a'),('é','e'),('ß','ss'),('Œ','OE'),('Å','A\u030a'),
             ('file2','file10'),('2','٠٢'),('1','１'),('a-b','ab'),('-\u0301a','a'),
             ('か','が'),('か','カ'),('カ','ｶ'),('カー','カア'),('キー','キイ'),
             ('かゝ','かか'),('はゞ','はば'),('Ω','Ω'),('あ','Ω'),('亜','唖'),
             ('\u0e40\u0e01','\u0e01\u0e40'),('x\u0327\u0301','x\u0301\u0327')]
    extras = [('\U00010000','\U00010001'),('\U0002ffff','\U00030000'),
              ('\U0010fffd','\U0010ffff'),('\u0378','\u0379'),('\ud800','\ud801'),
              ('\ufffd','\ud800'),('a\x00b','ab'),('a\u200db','ab'),
              ('가','가'),('각','각'),
              ('1'+'0'*299,'9'*299),('9'*254,'1'+'0'*254),
              ('0'*300+'2','2'),('x\u031b\u0323','x\u0323\u031b')]
    randomizer = random.Random(0x4e4f4354)
    alphabet = list('aAbBéÉœß̧́ あかがはばぱカガーゝ亜唖Ωα12٠١２😀') + ['\U0002ffff','\U0010ffff','\u0378']
    for _ in range(80):
        extras.append((''.join(randomizer.choices(alphabet, k=randomizer.randint(1,6))),
                       ''.join(randomizer.choices(alphabet, k=randomizer.randint(1,6)))))
    for locale, usage, sensitivity, numeric, case_first, punctuation in itertools.product(
            ['en','ja'], ['sort','search'], ['base','accent','case','variant'],
            [False,True], ['false','lower','upper'], [False,True]):
        error = C.c_int32()
        collator = icu.ucol_open((locale + ('@collation=search' if usage == 'search' else '')).encode('ascii'), C.byref(error))
        icu.checked(error)
        values = [(4,17), (5,{'base':0,'case':0,'accent':1,'variant':2}[sensitivity]),
                  (3,17 if sensitivity == 'case' else 16), (7,17 if numeric else 16),
                  (2,{'false':16,'lower':24,'upper':25}[case_first]), (1,20 if punctuation else 21)]
        for attribute, value in values:
            icu.ucol_setAttribute(collator, attribute, value, C.byref(error))
        icu.checked(error)
        chosen = list(pairs)
        if case_first == 'false' and punctuation is False:
            chosen.extend(extras)
        vectors = []
        for left, right in chosen:
            a, b = icu.text(left), icu.text(right)
            result = icu.ucol_strcoll(collator, a, len(a), b, len(b))
            vectors.append([left,right,result])
        groups.append({'locale':locale,'options':{'usage':usage,'sensitivity':sensitivity,
                       'numeric':numeric,'caseFirst':case_first,'ignorePunctuation':punctuation},'vectors':vectors})
        icu.ucol_close(collator)
    source = (VENDOR / 'test_cases.js').read_text(encoding='utf-8')
    source = source.replace('/* @collation_oracle */ null', json.dumps(groups, ensure_ascii=True, separators=(',',':')))
    path = ROOT / 'tests/js_collator_cases.js'
    path.write_text(source, encoding='utf-8', newline='\n')
    metadata = {'icu':icu.version,'groups':len(groups),'comparisonVectors':sum(len(g['vectors']) for g in groups),
                'seed':'0x4e4f4354','guestExecuted':False}
    (VENDOR / 'oracle.json').write_text(json.dumps(metadata, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(metadata))


if __name__ == '__main__':
    main()
