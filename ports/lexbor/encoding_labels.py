#!/usr/bin/env python3
"""Rewrites the label table of Lexbor's encoding module from the Encoding Standard.

lxb_encoding_res_shs_entities (third_party/lexbor/source/lexbor/encoding/res.c, its size in
res.h) is Lexbor's static hash of labels (core/shs.c, lexbor_shs_entry_get_lower_static): a
label of length n heads or joins the chain at slot
(l(k[0]) * l(k[n - 1]) * l(k[0]) + n) % size + 1, chains are in key length order, next 0 ends
one. Upstream generated it from an older encodings.json; this makes it from the standard's
current one, so every label the standard lists is found (NOCTURNE_PATCHES.json).

usage: python ports/lexbor/encoding_labels.py [ENCODINGS_JSON]
"""
from pathlib import Path
import json
import re
import sys

root = Path(__file__).resolve().parents[2]
res_c = root / "third_party/lexbor/source/lexbor/encoding/res.c"
res_h = root / "third_party/lexbor/source/lexbor/encoding/res.h"
source = Path(sys.argv[1] if len(sys.argv) > 1 else
              r"D:\Programes\cloned repos\Browser specs\whatwg specs\encoding\encodings.json")

text = res_c.read_bytes().decode("utf-8")
data = text[text.index("lxb_encoding_res_map[LXB_ENCODING_LAST_ENTRY]"):]
data = data[:data.index("};")]
position = {name: i for i, name in enumerate(re.findall(r'\(lxb_char_t \*\) "([^"]+)"\}', data))}

labels = []
for group in json.loads(source.read_text("utf-8")):
    for encoding in group["encodings"]:
        for label in encoding["labels"]:
            labels.append((label.lower(), position[encoding["name"]]))


def slot(key, size):
    a, b = key[0].lower(), key[-1].lower()
    return (ord(a) * ord(b) * ord(a) + len(key)) % size + 1


def chains(size):
    table = {}
    for key, value in labels:
        table.setdefault(slot(key, size), []).append((key, value))
    return table


# The smallest table whose longest chain is shortest.
size = min(range(len(labels), 4 * len(labels)), key=lambda s: (max(map(len, chains(s).values())), s))
entries = [None] * (size + 1)
overflow = []
for at, chain in sorted(chains(size).items()):
    chain.sort(key=lambda entry: (len(entry[0]), entry[0]))
    places = [at] + [size + 1 + len(overflow) + i for i in range(len(chain) - 1)]
    overflow.extend([None] * (len(chain) - 1))
    for i, (key, value) in enumerate(chain):
        entry = (key, value, places[i + 1] if i + 1 < len(places) else 0)
        if places[i] <= size:
            entries[places[i]] = entry
        else:
            overflow[places[i] - size - 1] = entry
entries += overflow

cells = ["{NULL, NULL, %d, 0}" % size]
for entry in entries[1:]:
    if entry is None:
        cells.append("{NULL, NULL, 0, 0}")
    else:
        key, value, next_ = entry
        cells.append('{"%s", (void *) &lxb_encoding_res_map[%d], %d, %d}' % (key, value, len(key), next_))
lines = ["    " + ", ".join(cells[i:i + 2]) + ("," if i + 2 < len(cells) else "") for i in range(0, len(cells), 2)]
table = ("LXB_API const lexbor_shs_entry_t lxb_encoding_res_shs_entities[%d] = \n{\n" % len(cells) +
         "\n".join(lines) + "\n};\n")

start = text.index("LXB_API const lexbor_shs_entry_t lxb_encoding_res_shs_entities[")
end = text.index("};", start) + 3
res_c.write_bytes((text[:start] + table + text[end:]).encode("utf-8"))
header = res_h.read_bytes().decode("utf-8")
header = re.sub(r"lxb_encoding_res_shs_entities\[\d+\]", "lxb_encoding_res_shs_entities[%d]" % len(cells), header)
res_h.write_bytes(header.encode("utf-8"))
print(f"{len(labels)} labels, {size} slots, {len(cells)} entries")
