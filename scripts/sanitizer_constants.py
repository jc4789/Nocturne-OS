"""Extract normative Sanitizer lists from a saved WHATWG source snapshot.

Usage: python -X utf8 scripts/sanitizer_constants.py SOURCE OUTPUT
No network access or build-time dependencies: the result is checked in.
"""
import hashlib
import json
import re
import sys
from pathlib import Path
from bs4 import BeautifulSoup

source_path, output_path = map(Path, sys.argv[1:])
source = source_path.read_text(encoding="utf-8")
namespaces = {"HTML": "http://www.w3.org/1999/xhtml", "SVG": "http://www.w3.org/2000/svg", "MathML": "http://www.w3.org/1998/Math/MathML"}
html_ns = namespaces["HTML"]
defaults, unsafe, navigating = [], [], []
for section in re.split(r"(?=<h[2-6]\b)", source):
    heading = re.match(r"<h[2-6]\b.*?</h[2-6]>", section, re.S)
    if not heading:
        continue
    hs = BeautifulSoup(heading[0], "html.parser")
    names = [n.get_text() for n in hs.select("dfn[element] code")]
    if not names:
        continue
    category = re.search(r'<dt><span data-x="concept-element-sanitization">.*?</dt>\s*<dd>(.*?)</dd>', section, re.S)
    if not category:
        continue
    cs = BeautifulSoup(category[1], "html.parser")
    # A navigating attribute is mentioned twice (allowed, then navigating).
    # It is one item in the normative per-element attribute set.
    attrs = list(dict.fromkeys(n.get_text() for n in cs.select("code")))
    for name in names:
        entry = {"name": name, "namespace": html_ns}
        if "sanitizer-category-default" in category[1]:
            defaults.append({**entry, "attributes": attrs, "removeAttributes": []})
        elif "sanitizer-category-unsafe" in category[1]:
            unsafe.append(entry)
        nav = re.search(r'<span>navigating URL attributes</span>(.*)', category[1], re.S)
        if nav:
            navigating += [[name, html_ns, n.get_text(), None] for n in BeautifulSoup(nav[1], "html.parser").select("code")]

tail = source[source.index("<dfn export>built-in safe default configuration</dfn>"):]
global_part = re.search(r'<dt><code data-x="dom-SanitizerConfig-attributes">.*?</dt>\s*<dd>(.*?)</dd>', tail, re.S)[1]
global_attrs = [n.get_text() for n in BeautifulSoup(global_part, "html.parser").select("li code")]
table_source = re.search(r"<table>(.*?)</table>", tail, re.S)[0]
# Source tables intentionally omit optional end tags; lxml recovers their cells.
table = BeautifulSoup(table_source, "lxml")
for row in table.select("tbody tr"):
    cells = row.find_all("td", recursive=False)
    if len(cells) != 3:
        raise ValueError("Foreign Sanitizer table row changed")
    ns = namespaces[cells[1].get_text(strip=True)]
    defaults.append({"name": cells[0].get_text(strip=True), "namespace": ns,
                     "attributes": [n.get_text() for n in cells[2].select("code")], "removeAttributes": []})
unsafe += [{"name": "frame", "namespace": html_ns}, {"name": "script", "namespace": namespaces["SVG"]}, {"name": "use", "namespace": namespaces["SVG"]}]
for item in defaults:
    item["attributes"] = [name for name in item["attributes"] if name not in global_attrs]
handlers = sorted(set(re.findall(r'<code[^>]*data-x="handler-[^"]+"[^>]*>(on[a-z]+)</code>', source)))
if len(defaults) < 100 or len(unsafe) < 5 or len(handlers) < 50:
    raise ValueError("Sanitizer extraction unexpectedly incomplete")
navtail=source[source.index("<dfn>built-in navigating URL attributes list</dfn>"):]
for row in BeautifulSoup(re.search(r"<table>(.*?)</table>", navtail,re.S)[0], "lxml").select("tbody tr"):
    cells=row.find_all("td",recursive=False)
    navigating.append([cells[0].get_text(strip=True),namespaces[cells[1].get_text(strip=True)],cells[2].get_text(strip=True),
                       "http://www.w3.org/1999/xlink" if cells[3].get_text(strip=True)=="XLink" else None])
data = {"elements": defaults, "attributes": global_attrs, "unsafeElements": unsafe, "eventAttributes": handlers, "navigatingAttributes": navigating}
output_path.write_text("/* Generated from WHATWG HTML source; sha256 " + hashlib.sha256(source.encode()).hexdigest() + "; scripts/sanitizer_constants.py. */\nconst sanitizerConstants=" + json.dumps(data, ensure_ascii=True, separators=(",", ":")) + ";\n", encoding="utf-8", newline="\n")
print(f"Sanitizer constants: {len(defaults)} default elements, {len(unsafe)} unsafe elements, {len(handlers)} handlers")
