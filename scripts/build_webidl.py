"""Generate user/libc/web/webidl_data.inc, the Web IDL table the browser's binding layer
(user/libc/web/js_webidl.c) installs interfaces by, from the specifications' IDL in
third_party/webref/idl.

The table keeps what decides an interface's JavaScript shape (Web IDL, "ECMAScript binding"):
which globals expose it, its inheritance, its constructor's length, and per member its kind,
static or not, the length of its shortest overload, the extended attributes that move or
lock it ([LegacyUnforgeable], [Replaceable], [PutForwards], [LegacyLenientSetter],
[Unscopable], [SecureContext], [CrossOriginIsolated]) and a constant's value. Types are not
kept: conversions belong to each implementation.

Usage: python -X utf8 scripts/build_webidl.py
"""
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IDL = ROOT / "third_party" / "webref" / "idl"
OUT = ROOT / "user" / "libc" / "web" / "webidl_data.inc"

TOKEN = re.compile(r"""
    (?P<space>\s+|//[^\n]*|/\*.*?\*/)
  | (?P<float>-?(?:(?:[0-9]+\.[0-9]*|[0-9]*\.[0-9]+)(?:[Ee][+-]?[0-9]+)?|[0-9]+[Ee][+-]?[0-9]+))
  | (?P<integer>-?(?:0[Xx][0-9A-Fa-f]+|0[0-7]*|[1-9][0-9]*))
  | (?P<string>"[^"]*")
  | (?P<ident>[_-]?[A-Za-z][0-9A-Z_a-z-]*)
  | (?P<other>\.\.\.|[^\s0-9A-Za-z])
""", re.S | re.X)


def tokens(text, name):
    out, at = [], 0
    while at < len(text):
        m = TOKEN.match(text, at)
        if not m:
            raise SyntaxError(f"{name}: cannot read IDL at {text[at:at + 40]!r}")
        at = m.end()
        if m.lastgroup != "space":
            out.append(m.group())
    return out


class Parser:
    def __init__(self, toks, name):
        self.t, self.i, self.name = toks, 0, name

    def peek(self, k=0):
        j = self.i + k
        return self.t[j] if j < len(self.t) else None

    def take(self, want=None):
        tok = self.peek()
        if tok is None or want is not None and tok != want:
            raise SyntaxError(f"{self.name}: expected {want!r}, found {tok!r} near {' '.join(self.t[self.i - 8:self.i + 8])}")
        self.i += 1
        return tok

    def balanced(self, open_, close):
        """Tokens between open_ and its matching close (both consumed)."""
        self.take(open_)
        depth, start = 1, self.i
        while depth:
            tok = self.take()
            if tok == open_:
                depth += 1
            elif tok == close:
                depth -= 1
        return self.t[start:self.i - 1]

    def ext_attrs(self):
        """[A, B=C, D=(E,F), G(args), H=I(args), J=*] as {name: value}."""
        if self.peek() != "[":
            return {}
        body = self.balanced("[", "]")
        attrs = {}
        for part in split_top(body):
            if not part:
                continue
            name, rest = part[0], part[1:]
            if not rest:
                attrs[name] = True
            elif rest[0] == "(":
                attrs[name] = ("args", rest[1:-1])
            elif rest[0] == "=":
                value = rest[1:]
                if value[0] == "(":
                    attrs[name] = [v for v in value[1:-1] if v != ","]
                elif len(value) > 1 and value[1] == "(":
                    attrs[name] = ("named", value[0], value[2:-1])
                else:
                    attrs[name] = value[0]
        return attrs


def split_top(toks):
    """Split at commas outside (), [], <> and {}."""
    parts, cur, depth = [], [], 0
    for tok in toks:
        if tok in "([<{":
            depth += 1
        elif tok in ")]>}":
            depth -= 1
        if tok == "," and depth == 0:
            parts.append(cur)
            cur = []
        else:
            cur.append(tok)
    parts.append(cur)
    return parts


def skip_ext(toks, i):
    if i < len(toks) and toks[i] == "[":
        depth = 0
        while True:
            if toks[i] == "[":
                depth += 1
            elif toks[i] == "]":
                depth -= 1
                if depth == 0:
                    return i + 1
            i += 1
    return i


def skip_type(toks, i):
    """Index after one IDL type starting at toks[i]."""
    i = skip_ext(toks, i)
    if toks[i] == "(":
        depth = 0
        while True:
            if toks[i] == "(":
                depth += 1
            elif toks[i] == ")":
                depth -= 1
                if depth == 0:
                    i += 1
                    break
            i += 1
    else:
        if toks[i] in ("unsigned", "unrestricted"):
            i += 1
        if toks[i] == "long" and i + 1 < len(toks) and toks[i + 1] == "long":
            i += 2
        else:
            i += 1
            if i < len(toks) and toks[i] == "<":
                depth = 0
                while True:
                    if toks[i] == "<":
                        depth += 1
                    elif toks[i] == ">":
                        depth -= 1
                        if depth == 0:
                            i += 1
                            break
                    i += 1
    if i < len(toks) and toks[i] == "?":
        i += 1
    return i


def required_count(args):
    """Web IDL: the length of an overload is its count of arguments before the first
    optional or variadic one."""
    count = 0
    for arg in split_top(args):
        if not arg:
            continue
        j = skip_ext(arg, 0)
        if arg[j] == "optional" or "..." in arg:
            break
        count += 1
    return count


def indexed_getter(args):
    """Whether a getter's argument list is (unsigned long index)."""
    parts = [p for p in split_top(args) if p]
    if len(parts) != 1:
        return False
    j = skip_ext(parts[0], 0)
    return parts[0][j:j + 2] == ["unsigned", "long"]


def const_value(toks):
    text = " ".join(toks)
    if text in ("true", "false"):
        return text == "true", True
    if text in ("Infinity", "-Infinity", "NaN"):
        return {"Infinity": "INFINITY", "-Infinity": "-INFINITY", "NaN": "NAN"}[text], False
    if re.fullmatch(r"-?0[Xx][0-9A-Fa-f]+", text):
        return int(text, 16), False
    if re.fullmatch(r"-?0[0-7]+", text):
        return int(text, 8), False
    if re.fullmatch(r"-?[0-9]+", text):
        return int(text), False
    return float(text), False


class Member:
    def __init__(self, kind, name, attrs, static=False):
        self.kind, self.name, self.attrs, self.static = kind, name, attrs, static
        self.length = 0
        self.readonly = False
        self.value = None
        self.boolean = False
        self.iterator = None  # "sync" / "async": this member is also @@iterator / @@asyncIterator
        self.array = False  # a value iterator's member: %Array.prototype% holds it
        self.same_as_values = False  # a setlike's keys: the function its values is
        self.exposed = None
        self.secure = False
        self.isolated = False


class Definition:
    def __init__(self, kind, name):
        self.kind, self.name = kind, name
        self.parent = None
        self.attrs = {}
        self.members = []
        self.ctor = None  # length of the shortest constructor overload, if any
        self.indexed = False
        self.iterable = None  # (kind, readonly, pair, args)


def parse_members(p, owner, body_attrs):
    while p.peek() != "}":
        attrs = p.ext_attrs()
        start = p.i
        depth = 0
        while True:
            tok = p.take()
            if tok in "(<{":
                depth += 1
            elif tok in ")>}":
                depth -= 1
            elif tok == ";" and depth == 0:
                break
        toks = p.t[start:p.i - 1]
        member(owner, toks, attrs)


def member(owner, toks, attrs):
    static = False
    if toks[0] == "const":
        name = toks[toks.index("=") - 1]
        m = Member("const", name, attrs)
        m.value, m.boolean = const_value(toks[toks.index("=") + 1:])
        owner.members.append(m)
        return
    if toks[0] == "constructor":
        length = required_count(toks[2:-1])
        owner.ctor = length if owner.ctor is None else min(owner.ctor, length)
        return
    if toks[0] == "stringifier":
        rest = toks[1:]
        m = Member("op", "toString", attrs)
        owner.members.append(m)
        if not rest:
            return
        toks = rest
    if toks[0] == "static":
        static = True
        toks = toks[1:]
    readonly = False
    if toks[0] == "readonly":
        readonly = True
        toks = toks[1:]
    if toks[0] == "inherit":
        toks = toks[1:]
    if toks[0] == "attribute":
        m = Member("attr", toks[-1], attrs, static)
        m.readonly = readonly
        owner.members.append(m)
        return
    if toks[0] in ("iterable", "maplike", "setlike", "async", "async_iterable"):
        kind = toks[0]
        i = 1
        if kind == "async":
            kind = "async_iterable"
            i = 2
        close = i
        depth = 0
        while True:
            if toks[close] == "<":
                depth += 1
            elif toks[close] == ">":
                depth -= 1
                if depth == 0:
                    break
            close += 1
        pair = len([x for x in split_top(toks[i + 1:close]) if x]) == 2
        args = toks[close + 2:-1] if close + 1 < len(toks) and toks[close + 1] == "(" else []
        owner.iterable = (kind, readonly, pair, args)
        return
    special = False
    getter = False
    while toks[0] in ("getter", "setter", "deleter"):
        special = True
        getter = getter or toks[0] == "getter"
        toks = toks[1:]
    end = skip_type(toks, 0)
    if toks[end] == "(":
        if getter and indexed_getter(toks[end + 1:-1]):
            owner.indexed = True
        return  # an unnamed special operation has no property
    name = toks[end]
    args = toks[end + 2:-1]
    if getter and indexed_getter(args):
        owner.indexed = True
    m = Member("op", name, attrs, static)
    m.length = required_count(args)
    owner.members.append(m)
    _ = special


def parse(path):
    text = path.read_text(encoding="utf-8")
    p = Parser(tokens(text, path.name), path.name)
    defs, includes = [], []
    while p.peek() is not None:
        attrs = p.ext_attrs()
        partial = False
        if p.peek() == "partial":
            partial = True
            p.take()
        kind = p.take()
        if kind == "callback":
            if p.peek() == "interface":
                p.take()
                d = Definition("callback", p.take())
                d.attrs = attrs
                d.partial = partial
                p.take("{")
                parse_members(p, d, attrs)
                p.take("}")
                p.take(";")
                defs.append(d)
            else:
                while p.take() != ";":
                    pass
            continue
        if kind in ("interface", "namespace"):
            if kind == "interface" and p.peek() == "mixin":
                p.take()
                kind = "mixin"
            d = Definition(kind, p.take())
            d.attrs = attrs
            d.partial = partial
            if p.peek() == ":":
                p.take()
                d.parent = p.take()
            p.take("{")
            parse_members(p, d, attrs)
            p.take("}")
            p.take(";")
            defs.append(d)
            continue
        if kind in ("dictionary", "enum"):
            p.take()
            if p.peek() == ":":
                p.take()
                p.take()
            p.balanced("{", "}")
            p.take(";")
            continue
        if kind == "typedef":
            while p.take() != ";":
                pass
            continue
        if p.peek() == "includes":
            p.take()
            includes.append((kind, p.take()))
            p.take(";")
            continue
        raise SyntaxError(f"{path.name}: unknown definition {kind!r}")
    return defs, includes


def names(value):
    if value is True or value is None:
        return None
    return value if isinstance(value, list) else [value]


def main():
    manifest = json.loads((IDL.parent / "upstream.json").read_text(encoding="utf-8"))
    interfaces, mixins, includes = {}, {}, []
    partials = []
    for path in sorted(IDL.glob("*.idl")):
        if hashlib.sha256(path.read_bytes()).hexdigest() != manifest["files"].get(path.name):
            raise ValueError(f"{path.name} is not the file third_party/webref/upstream.json names")
        defs, inc = parse(path)
        includes += inc
        for d in defs:
            table = mixins if d.kind == "mixin" else interfaces
            if d.partial:
                partials.append(d)
            elif d.name in table:
                raise ValueError(f"{d.name} is defined twice")
            else:
                table[d.name] = d
    # Partial definitions add their members, each carrying the partial's [Exposed] and
    # [SecureContext] unless the member has its own.
    for d in partials:
        table = mixins if d.kind == "mixin" else interfaces
        whole = table.get(d.name)
        if whole is None:
            continue  # a partial of a definition outside these specifications
        for m in d.members:
            if "Exposed" not in m.attrs and "Exposed" in d.attrs:
                m.attrs["Exposed"] = d.attrs["Exposed"]
            if "SecureContext" in d.attrs:
                m.attrs.setdefault("SecureContext", True)
            if "CrossOriginIsolated" in d.attrs:
                m.attrs.setdefault("CrossOriginIsolated", True)
        whole.members += d.members
        if d.ctor is not None:
            whole.ctor = d.ctor if whole.ctor is None else min(whole.ctor, d.ctor)
        whole.indexed = whole.indexed or d.indexed
        whole.iterable = whole.iterable or d.iterable

    # The globals: each [Global] interface and the names it answers to.
    globals_ = []
    for d in interfaces.values():
        if "Global" in d.attrs:
            globals_.append((d.name, names(d.attrs["Global"]) or [d.name]))
    globals_.sort(key=lambda g: (g[0] != "Window", g[0]))
    if len(globals_) > 16:
        raise ValueError("more than 16 globals")
    every = (1 << len(globals_)) - 1

    def exposure(value):
        if value is None:
            return None
        if value == "*":
            return every
        mask = 0
        for name in names(value):
            for bit, (_, answers) in enumerate(globals_):
                if name in answers:
                    mask |= 1 << bit
        return mask

    # Members of included mixins, exposed where both the member (or its mixin) and the
    # including interface are.
    for target, mixin in includes:
        if target not in interfaces or mixin not in mixins:
            continue
        m_def = mixins[mixin]
        for m in m_def.members:
            copy = Member(m.kind, m.name, dict(m.attrs), m.static)
            copy.__dict__.update({k: v for k, v in m.__dict__.items() if k != "attrs"})
            if "Exposed" not in copy.attrs and "Exposed" in m_def.attrs:
                copy.attrs["Exposed"] = m_def.attrs["Exposed"]
            if "SecureContext" in m_def.attrs:
                copy.attrs.setdefault("SecureContext", True)
            interfaces[target].members.append(copy)

    out_members, out_interfaces = [], []
    order = sorted(interfaces.values(), key=lambda d: d.name)
    index = {d.name: i for i, d in enumerate(order)}
    for d in order:
        if d.kind == "callback" and not any(m.kind == "const" for m in d.members):
            continue  # a callback interface without constants has no interface object
        own = exposure(d.attrs.get("Exposed"))
        if own is None:
            own = every if d.kind != "callback" else exposure("Window")
        members = synthesize(d)
        merged = {}
        for m in members:
            m.exposed = own if m.attrs.get("Exposed") is None else exposure(m.attrs["Exposed"]) & own
            m.secure = "SecureContext" in m.attrs or "SecureContext" in d.attrs
            m.isolated = "CrossOriginIsolated" in m.attrs or "CrossOriginIsolated" in d.attrs
            key = (m.kind if m.kind != "op" else "op", m.name, m.static)
            if key in merged:
                first = merged[key]
                first.length = min(first.length, m.length)
                first.exposed |= m.exposed
                first.secure = first.secure and m.secure
                first.iterator = first.iterator or m.iterator
                continue
            merged[key] = m
        first = len(out_members)
        out_members += merged.values()
        factory = d.attrs.get("LegacyFactoryFunction")
        out_interfaces.append({
            "name": d.name, "kind": d.kind, "parent": index.get(d.parent, -1) if d.parent else -1,
            "exposed": own, "first": first, "count": len(merged),
            "ctor": d.ctor, "global": "Global" in d.attrs,
            "secure": "SecureContext" in d.attrs, "isolated": "CrossOriginIsolated" in d.attrs,
            "nointerface": "LegacyNoInterfaceObject" in d.attrs,
            "alias": names(d.attrs.get("LegacyWindowAlias")) or [],
            "factory": (factory[1], required_count(factory[2])) if isinstance(factory, tuple) else None,
            "namespace": d.attrs.get("LegacyNamespace"),
        })
    # Parents refer into out_interfaces, whose order follows `order` minus skipped callbacks.
    position = {iface["name"]: i for i, iface in enumerate(out_interfaces)}
    for iface in out_interfaces:
        if iface["parent"] >= 0:
            iface["parent"] = position.get(order[iface["parent"]].name, -1)
    write(globals_, out_interfaces, out_members, manifest)


def synthesize(d):
    """The members a definition's iterable, maplike or setlike declaration adds."""
    members = list(d.members)
    if not d.iterable:
        return members
    kind, readonly, pair, args = d.iterable

    def op(name, length=0, iterator=None):
        m = Member("op", name, {})
        m.length = length
        m.iterator = iterator
        m.array = kind == "iterable" and not pair
        members.append(m)

    if kind == "iterable":
        if pair:
            op("entries", 0, "sync"); op("keys"); op("values"); op("forEach", 1)
        else:
            op("entries"); op("keys"); op("values", 0, "sync"); op("forEach", 1)
    elif kind == "async_iterable":
        length = required_count(args)
        if pair:
            op("entries", length, "async"); op("keys", length); op("values", length)
        else:
            op("values", length, "async")
    else:
        size = Member("attr", "size", {})
        size.readonly = True
        members.append(size)
        if kind == "maplike":
            op("entries", 0, "sync"); op("keys"); op("values"); op("forEach", 1); op("get", 1); op("has", 1)
            if not readonly:
                op("set", 2); op("delete", 1); op("clear")
        else:
            op("entries"); op("keys"); op("values", 0, "sync"); op("forEach", 1); op("has", 1)
            members[-4].same_as_values = True
            if not readonly:
                op("add", 1); op("delete", 1); op("clear")
    return members


def c_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def c_number(value):
    if isinstance(value, str):
        return value
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, int):
        return f"{value}.0"
    return repr(value)


def write(globals_, interfaces, members, manifest):
    kinds = {"const": "WEBIDL_CONST", "attr": "WEBIDL_ATTRIBUTE", "op": "WEBIDL_OPERATION"}
    lines = [
        "/* Generated by scripts/build_webidl.py from third_party/webref/idl (the Web IDL of the",
        f"   W3C and WHATWG specifications, web-platform-tests {manifest['wpt_commit'][:12]}). Do not edit. */",
        f"#define WEBIDL_GLOBAL_COUNT {len(globals_)}",
    ]
    for bit, (name, _) in enumerate(globals_):
        words = re.sub(r"(?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z])", "_", name.replace("GlobalScope", ""))
        lines.append(f"#define WEBIDL_GLOBAL_{words.upper()} (1u << {bit})")
    lines.append("static const char *const webidl_global_names[WEBIDL_GLOBAL_COUNT] = {")
    lines.append("    " + ", ".join(c_string(name) for name, _ in globals_) + ",")
    lines.append("};")
    lines.append(f"#define WEBIDL_MEMBER_COUNT {len(members)}")
    lines.append("static const struct webidl_member webidl_members[WEBIDL_MEMBER_COUNT] = {")
    for m in members:
        flags = []
        if m.static:
            flags.append("WEBIDL_STATIC")
        if m.readonly:
            flags.append("WEBIDL_READONLY")
        for attr, flag in (("LegacyUnforgeable", "WEBIDL_UNFORGEABLE"), ("Replaceable", "WEBIDL_REPLACEABLE"),
                           ("PutForwards", "WEBIDL_PUT_FORWARDS"), ("LegacyLenientSetter", "WEBIDL_LENIENT_SETTER"),
                           ("Unscopable", "WEBIDL_UNSCOPABLE"), ("LegacyLenientThis", "WEBIDL_LENIENT_THIS")):
            if attr in m.attrs:
                flags.append(flag)
        if m.secure:
            flags.append("WEBIDL_SECURE")
        if m.isolated:
            flags.append("WEBIDL_ISOLATED")
        if m.boolean:
            flags.append("WEBIDL_BOOLEAN")
        if m.iterator == "sync":
            flags.append("WEBIDL_ITERATOR")
        if m.iterator == "async":
            flags.append("WEBIDL_ASYNC_ITERATOR")
        if m.array:
            flags.append("WEBIDL_ARRAY_ITERATOR")
        if m.same_as_values:
            flags.append("WEBIDL_SAME_AS_VALUES")
        value = c_number(m.value) if m.kind == "const" else "0"
        lines.append(f"    {{{c_string(m.name)}, {kinds[m.kind]}, {'|'.join(flags) or '0'}, {m.length}, 0x{m.exposed:x}, {value}}},")
    lines.append("};")
    lines.append(f"#define WEBIDL_INTERFACE_COUNT {len(interfaces)}")
    lines.append("static const struct webidl_interface webidl_interfaces[WEBIDL_INTERFACE_COUNT] = {")
    for d in interfaces:
        flags = []
        if d["kind"] == "namespace":
            flags.append("WEBIDL_IS_NAMESPACE")
        if d["kind"] == "callback":
            flags.append("WEBIDL_IS_CALLBACK")
        if d["global"]:
            flags.append("WEBIDL_IS_GLOBAL")
        if d["secure"]:
            flags.append("WEBIDL_IS_SECURE")
        if d["isolated"]:
            flags.append("WEBIDL_IS_ISOLATED")
        if d["nointerface"]:
            flags.append("WEBIDL_NO_INTERFACE_OBJECT")
        if d["ctor"] is not None:
            flags.append("WEBIDL_CONSTRUCTOR")
        alias = c_string(",".join(d["alias"])) if d["alias"] else "NULL"
        factory, factory_length = (c_string(d["factory"][0]), d["factory"][1]) if d["factory"] else ("NULL", 0)
        space = c_string(d["namespace"]) if d["namespace"] else "NULL"
        lines.append(f"    {{{c_string(d['name'])}, {d['parent']}, {'|'.join(flags) or '0'}, 0x{d['exposed']:x}, {d['ctor'] or 0}, "
                     f"{d['first']}, {d['count']}, {alias}, {factory}, {factory_length}, {space}}},")
    lines.append("};")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"build_webidl: {len(interfaces)} interfaces, {len(members)} members, {len(globals_)} globals -> {OUT.relative_to(ROOT)}")


if __name__ == "__main__":
    try:
        main()
    except (SyntaxError, ValueError) as error:
        sys.exit(f"build_webidl: {error}")
