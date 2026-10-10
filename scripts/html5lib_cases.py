#!/usr/bin/env python3
"""Generate native-product cases from official html5lib/WPT .dat corpora.

No expected trees are rewritten, and parse-error counts are retained as
metadata, not claimed as checked. Paths on the command line must be absolute.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path

MARKERS = {"#data", "#description", "#errors", "#new-errors", "#document-fragment",
           "#script-on", "#script-off", "#document"}


def parse_file(path: Path) -> list[dict]:
    raw = path.read_bytes()
    text = raw.decode("utf-8")
    if not text.endswith("\n"):
        raise ValueError(f"{path}: corpus must end with LF")
    # splitlines() would corrupt literal FF/CR in tokenizer inputs.
    lines = text.split("\n")[:-1]
    result, i = [], 0
    while i < len(lines):
        if not lines[i]:
            i += 1
            continue
        start = i + 1
        description = []
        if lines[i] == "#description":
            i += 1
            while i < len(lines) and lines[i] != "#data":
                description.append(lines[i]); i += 1
        if i >= len(lines) or lines[i] != "#data":
            raise ValueError(f"{path}:{i+1}: expected #data")
        i += 1
        data = []
        while i < len(lines) and lines[i] != "#errors":
            data.append(lines[i]); i += 1
        if i == len(lines):
            raise ValueError(f"{path}:{start}: missing #errors")
        i += 1
        errors = 0
        while i < len(lines) and lines[i] not in MARKERS:
            if not lines[i]:
                raise ValueError(f"{path}:{i+1}: empty parse error")
            errors += 1; i += 1
        if i < len(lines) and lines[i] == "#new-errors":
            i += 1
            while i < len(lines) and lines[i] not in MARKERS:
                if not lines[i]:
                    raise ValueError(f"{path}:{i+1}: empty new parse error")
                errors += 1; i += 1
        context, namespace, scripting = "", 0, None
        if i < len(lines) and lines[i] == "#document-fragment":
            i += 1
            if i == len(lines) or not lines[i] or lines[i].startswith("#"):
                raise ValueError(f"{path}:{i+1}: missing fragment context")
            context = lines[i]; i += 1
            for prefix, ns in (("svg ", 1), ("math ", 2)):
                if context.startswith(prefix):
                    context, namespace = context[len(prefix):], ns
                    break
        if i < len(lines) and lines[i] in ("#script-on", "#script-off"):
            scripting = lines[i] == "#script-on"; i += 1
        if i == len(lines) or lines[i] != "#document":
            raise ValueError(f"{path}:{i+1}: expected #document")
        i += 1
        expected = []
        while i < len(lines) and lines[i] not in ("#data", "#description"):
            expected.append(lines[i]); i += 1
        # Remove the one empty separator line, not input/expected whitespace.
        if expected and expected[-1] == "":
            expected.pop()
        tree = "\n".join(expected) + ("\n" if expected else "")
        if expected and not expected[0].startswith("| "):
            raise ValueError(f"{path}:{start}: malformed expected tree")
        result.append({"id": f"{path.name}:{len(result)+1}", "line": start,
                       "data": "\n".join(data), "expected": tree,
                       "context": context, "namespace": namespace,
                       "scripting": scripting, "errors": errors,
                       "description": "\n".join(description)})
    return result


def c_string(value: str) -> str:
    pieces, chunk = [], ""
    for byte in value.encode("utf-8"):
        token = {10: r"\n", 13: r"\r", 9: r"\t", 34: r'\"', 92: r"\\"}.get(byte)
        if token is None:
            token = chr(byte) if 32 <= byte < 127 else f"\\{byte:03o}"
        if len(chunk) + len(token) > 100:
            pieces.append('"' + chunk + '"'); chunk = ""
        chunk += token
    pieces.append('"' + chunk + '"')
    return "\n        ".join(pieces)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("corpus", type=Path, nargs="+")
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--manifest", type=Path, required=True)
    ap.add_argument("--skip-legacy-pi", action="append", default=[], metavar="BASENAME:CASE")
    args = ap.parse_args()
    for path in [*args.corpus, args.output, args.manifest]:
        if not path.is_absolute():
            ap.error(f"absolute path required: {path}")
    cases, files = [], []
    for path in args.corpus:
        cases.extend(parse_file(path))
        raw = path.read_bytes()
        files.append({"path": str(path), "sha256": hashlib.sha256(raw).hexdigest(), "bytes": len(raw)})
    ids = [case["id"] for case in cases]
    if len(set(ids)) != len(ids):
        ap.error("corpus basenames must be unique")
    skip = set(args.skip_legacy_pi)
    if skip - set(ids):
        ap.error("unknown skip case: " + ", ".join(sorted(skip - set(ids))))
    for case in cases:
        if case["id"] in skip and ("<?" not in case["data"] or "<?" in case["expected"]):
            ap.error("legacy-PI skip requires PI input with pre-PI expected tree: " + case["id"])
    out = ["/* Generated by scripts/html5lib_cases.py; expected trees unchanged. */",
           "#ifndef NOCTURNE_HTML5LIB_CASES_H", "#define NOCTURNE_HTML5LIB_CASES_H",
           "struct html5lib_case { const char *id,*data,*expected,*context,*skip;",
           "    size_t data_length,expected_length; unsigned namespace_id,error_count; bool scripting; };",
           "static const struct html5lib_case html5lib_cases[] = {"]
    expanded = []
    for case in cases:
        modes = [False, True] if case["scripting"] is None else [case["scripting"]]
        reason = "legacy PI expectation predates current HTML ProcessingInstruction" if case["id"] in skip else ""
        for mode in modes:
            out.extend(["    {" + c_string(case["id"] + (":script-on" if mode else ":script-off")) + ",",
                        "        " + c_string(case["data"]) + ",", "        " + c_string(case["expected"]) + ",",
                        "        " + c_string(case["context"]) + ", " + c_string(reason) + ",",
                        f"        {len(case['data'].encode('utf-8'))}, {len(case['expected'].encode('utf-8'))}, "
                        f"{case['namespace']}, {case['errors']}, {'true' if mode else 'false'}" + "},"])
            expanded.append({"id": case["id"], "line": case["line"], "scripting": mode,
                             "context": case["context"], "namespace": case["namespace"],
                             "skip": reason, "expected_errors_unchecked": case["errors"]})
    out.extend(["};", "#endif", ""])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(out), encoding="utf-8", newline="\n")
    args.manifest.write_text(json.dumps({"files": files, "source_cases": len(cases),
        "generated_cases": len(expanded), "skipped_cases": sum(bool(c["skip"]) for c in expanded),
        "parse_error_counts_checked": False, "cases": expanded}, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"html5lib: {len(cases)} source cases, {len(expanded)} mode cases, {sum(bool(c['skip']) for c in expanded)} explicit skips")


if __name__ == "__main__":
    main()
