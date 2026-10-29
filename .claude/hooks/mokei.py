"""長い作業の作業場を保つフック（SessionStart・UserPromptSubmit・PostToolUse・Stop）。

作業場は、書く人で分ける。
- 模型：本人が書く（.claude/mokei/<作業場>.md）。今、頭に置く判断。
- 会話の記録：このフックが事象から書く（<作業場>.jsonl）。ユーザーの原文、模型のすべての版、
  編集したファイル、計測（serial.log を読んだ・撮った・QEMU を起こしたこと）、止めた命令、圧縮、自分の報告。
  圧縮や再開の後に、要約とは別に、起きたことを見せる。
- コードの覚え書き：.claude/oboegaki.md。ファイルに結び付き、そのファイルに触れた時にここが出す。
- コードのつながり：codegraph の索引（.codegraph/）。編集した関数を呼ぶ所を、編集の直後に一度だけ出す。

ここは見せるだけで、作業を止めない。止めるのは二つ：ユーザーへの返事に日本語が無いとき（Stop）と、
ユーザーが禁じた命令（PreToolUse。窓なしの QEMU、三台目の QEMU、三つ目の媒体、build の下の記録、
窓なしのままの試験の道具、目標の無いビルド。2026-10-29）。
使い方は .claude/skills/mokei/SKILL.md にある。失敗は .claude/mokei/errors.log に残し、会話には出さない。
"""
import datetime
import fnmatch
import json
import os
import re
import shutil
import subprocess
import sys
import traceback
from collections import Counter
from pathlib import Path

# アプリや道具やほかの担当が会話へ送る知らせ。ユーザーの発言ではない。
MACHINE = ("<task-notification", "<side-sessions-event", "<ci-monitor-event", "<system-reminder",
           "<local-command", "<command-name>", "[SYSTEM NOTIFICATION", "<agent-message",
           "[Subagent hand-back]", "Another Claude session sent a message",
           "This session is being continued from a previous conversation", "[Artifact comment")
EDITS = {"Edit", "Write", "MultiEdit", "NotebookEdit"}
SHELLS = {"Bash", "PowerShell"}
# シェルの命令にこれがあれば、物差しを測った（ログを読んだ・撮った・QEMU を起こした・対象を絞った WPT を回した）と数える。
# platform_qa と記録のフォルダー（hikaku）は数えない：10-28〜29 の会話では「測った」の大半がそれらで、
# 本物（媒体を窓付きで見る）から離れた。
MEASURE = ("serial.log", "screendump", "qemu-system", "hv-serial", "hv-boot", "System.log",
           "browser log", "browser trace", "wpt.py", "scripts/test.py", "scripts\\test.py")
# 実行の前に止める命令（2026-10-29 ユーザーの指示）。読むだけの命令（grep・ls など）は build の道の規則から除く。
READ_ONLY = ("grep", "rg ", "git ", "ls", "cat ", "find ", "head ", "tail ", "sed -n", "wc ", "stat ", "dir ",
             "Get-ChildItem", "Get-Content", "Select-String", "Test-Path", "Get-Item")
FORBIDDEN = (
    (re.compile(r"qemu-system[^\n;&|]*(-display\s+(none|egl-headless)|-nographic)"),
     "QEMU を窓なしで起動する命令。QEMU は窓付きで二台まで（nocturne-project スキル）。ユーザーはその窓を見ている。", False),
    (re.compile(r"python[^\n]*scripts[/\\](platform_qa|qtest)\.py"),
     "媒体を何枚も写して窓なしの QEMU を起こす道具。使わない。試験は wpt.py（対象を絞る）と test.py を窓付きに直して使う。", False),
    (re.compile(r"build[/\\](qa|nocturne-platform)\b"),
     "build の下の記録のフォルダー。build には今のビルドの出力と試験の道具の出力（build/wpt）だけ。画像とログは会話の作業場（scratchpad）へ。", True),
    (re.compile(r"python[^\n]*scripts[/\\]build\.py(?:\s+(?:-j|--jobs)\s*\d+)?\s*(?:2>&1\s*)?(?:$|[|;&])", re.M),
     "目標の無いビルド（既定は all で vhd・vhdx・iso まで作る）。確かめは qemu-image、Hyper-V へ渡すときだけ all と書く。", False),
)
# 数で止めるもの（2026-10-29 夕、ユーザー）：QEMU は同時に二台まで（何台も起こして RAM が尽き、ページングに落ちて
# 速さの値が歪んだ）、build の下の媒体（.img）は本線と写し一つの二つまで。試験の道具は、窓なしの印が残る間は止める。
MAX_QEMU = 2
MAX_IMAGES = 2
TOOL_RUN = re.compile(r"python[^\n]*scripts[/\\](test|wpt)\.py")
TOOL_IMAGE = {"test.py": "test.img", "wpt.py": "wpt.img"}  # 道具が自分で作る写し
HEADLESS = re.compile(r'-display"[^\n]*"(none|egl-headless)"|CREATE_NO_WINDOW')
COPY_IMAGE = re.compile(r"(\bcp\b|\bcopy\b|Copy-Item|robocopy|qemu-img\s+convert)[^\n]*nocturne\.img")
NOTES = Path(".claude") / "oboegaki.md"
CODE = {".c", ".h", ".inc", ".cc", ".cpp", ".cxx", ".hh", ".hpp",
        ".js", ".mjs", ".cjs", ".jsx", ".ts", ".mts", ".cts", ".tsx", ".rs", ".go"}
SURE = 0.8  # これより確からしさの低いつながり（名前だけで推したもの）は出さない
QUIET = datetime.timedelta(hours=2)  # ユーザーのいない時間がこれを越えるごとに、節目の報告を促す
MANY_EDITS = 40  # 模型を書かずにこれだけ編集したら、模型を今に合わせるよう促す
MANY_MEASURES = 12  # 編集なしに測る命令がこれだけ続いたら、読んで直す側へ戻るよう促す。測る回は欠陥を直さない
JA = re.compile(r"[぀-ヿ一-鿿]")


def now():
    return datetime.datetime.now().astimezone()


def clock(when):
    return when.astimezone().strftime("%m-%d %H:%M")


def since(when):
    if not when:
        return "不明"
    minutes = int((now() - when).total_seconds() // 60)
    if minutes >= 48 * 60:
        return f"{minutes // 1440} 日 {minutes % 1440 // 60} 時間"
    return f"{minutes // 60} 時間 {minutes % 60} 分" if minutes >= 60 else f"{minutes} 分"


def shorten(text, limit):
    text = text.strip()
    return text if len(text) <= limit else text[:limit] + f"…（残り {len(text) - limit} 字）"


def one_line(text, limit):
    return shorten(" ".join(text.split()), limit)


def git(root, *args):
    try:
        done = subprocess.run(["git", "-C", str(root), *args], capture_output=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return done.stdout.decode("utf-8", "replace") if done.returncode == 0 else None


class Workspace:
    """一つの会話の作業場。デスクトップのアプリでは、再開で会話の識別子が変わっても同じ作業場を使う。"""

    def __init__(self, data):
        self.root = Path(os.environ.get("CLAUDE_PROJECT_DIR") or data.get("cwd") or ".")
        self.folder = self.root / ".claude" / "mokei"
        self.folder.mkdir(parents=True, exist_ok=True)
        ignore = self.folder / ".gitignore"
        if not ignore.exists():
            ignore.write_bytes(b"*\n")
        session = data.get("session_id") or "unknown"
        key = os.environ.get("CLAUDE_CODE_HOST_SESSION_ID") or session
        self.model = self.folder / f"{key}.md"
        self.log = self.folder / f"{key}.jsonl"
        self.state_path = self.folder / f"{key}.state.json"
        earlier = self.folder / f"{session}.md"
        if key != session and earlier.is_file() and not self.model.exists():
            earlier.rename(self.model)

    def is_model(self, path):
        return os.path.normcase(os.path.abspath(path)) == os.path.normcase(os.path.abspath(self.model))

    def relative(self, path):
        try:
            rel = os.path.relpath(os.path.abspath(path), os.path.abspath(self.root)).replace("\\", "/")
        except ValueError:
            return None
        return None if rel == ".." or rel.startswith("../") else rel

    def state(self):
        try:
            return json.loads(self.state_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}

    def save(self, state):
        temporary = self.state_path.with_name(f"{self.state_path.name}.{os.getpid()}")
        temporary.write_bytes(json.dumps(state, ensure_ascii=False).encode("utf-8"))
        os.replace(temporary, self.state_path)

    def record(self, kind, **fields):
        line = json.dumps({"t": now().isoformat(timespec="seconds"), "k": kind, **fields}, ensure_ascii=False)
        with open(self.log, "a", encoding="utf-8", newline="\n") as log:
            log.write(line + "\n")

    def events(self):
        found = []
        try:
            with open(self.log, encoding="utf-8") as log:
                for line in log:
                    try:
                        event = json.loads(line)
                        event["t"] = datetime.datetime.fromisoformat(event["t"])
                    except (ValueError, KeyError, TypeError):
                        continue
                    found.append(event)
        except OSError:
            pass
        return found

    def model_text(self):
        try:
            return self.model.read_text(encoding="utf-8").strip()
        except OSError:
            return ""


def last_time(events, kind):
    return max((event["t"] for event in events if event["k"] == kind), default=None)


def count_after(events, kind, when):
    return sum(1 for event in events if event["k"] == kind and (when is None or event["t"] > when))


class Note:
    """コードの覚え書き一つ。対象はリポジトリからの道（glob、末尾の / は下全部）。"""

    def __init__(self, title, targets, checked, commit, body):
        self.title, self.targets, self.checked, self.commit, self.body = title, targets, checked, commit, body

    def matches(self, rel):
        return any(fnmatch.fnmatch(rel, target + "*" if target.endswith("/") else target)
                   for target in self.targets)

    def freshness(self, root):
        specs = [":(glob)" + (target + "**" if target.endswith("/") else target) for target in self.targets]
        parts = [f"確かめた {self.checked or '日付なし'}"]
        if self.commit:
            count = git(root, "rev-list", "--count", f"{self.commit}..HEAD", "--", *specs)
            parts.append(f"その後この対象に入ったコミット {count.strip()}" if count is not None
                         else f"確かめたコミット {self.commit} が見つからない")
        dirty = git(root, "status", "--porcelain", "--", *specs)
        if dirty and dirty.strip():
            parts.append(f"未コミットの変更 {len(dirty.splitlines())} ファイル")
        return "、".join(parts)


def notes(root):
    try:
        text = (root / NOTES).read_text(encoding="utf-8")
    except OSError:
        return []
    text = re.sub(r"^```.*?^```", "", text, flags=re.M | re.S)
    found = []
    for block in re.split(r"^## ", text, flags=re.M)[1:]:
        title, _, rest = block.partition("\n")
        targets, checked, commit, body = [], "", "", []
        for line in rest.splitlines():
            field = re.match(r"(対象|確かめた)\s*[:：]\s*(.*)", line.strip())
            if not field:
                body.append(line)
            elif field.group(1) == "対象":
                targets = [target.strip() for target in re.split(r"[,、]", field.group(2)) if target.strip()]
            else:
                words = field.group(2).split()
                checked = words[0] if words else ""
                commit = words[1] if len(words) > 1 else ""
        if targets:
            found.append(Note(title.strip(), targets, checked, commit, "\n".join(body).strip()))
    return found


class Graph:
    """codegraph の索引を、その CLI で引く。索引か CLI が無ければ何も返さない。"""

    def __init__(self, root):
        self.root = root
        self.exe = shutil.which("codegraph") if (root / ".codegraph").is_dir() else None

    def run(self, *args):
        if not self.exe:
            return None
        try:
            done = subprocess.run([self.exe, *args], cwd=self.root, capture_output=True, timeout=5,
                                  env=dict(os.environ, DO_NOT_TRACK="1", NO_COLOR="1"))
        except (OSError, subprocess.TimeoutExpired):
            return None
        return done.stdout.decode("utf-8", "replace") if done.returncode == 0 else None

    def file(self, rel):
        """そのファイルに頼るファイルの一行と、記号の（名前, 種類, 始まりの行）。"""
        text = self.run("node", "-f", rel, "--symbols-only")
        if not text or "symbols" not in text.split("\n", 1)[0]:
            return None, []
        users = re.search(r"used by (\d+) files?: (.*)$", text.split("\n", 1)[0])
        symbols = [(m.group(1), m.group(2), int(m.group(3)))
                   for m in re.finditer(r"^- `(.+?)` \((\w+)\).*:(\d+)\s*$", text, re.M)]
        return (int(users.group(1)), users.group(2)) if users else (0, ""), symbols

    def callers(self, name, rel, limit=40):
        """確かなつながりで呼ぶ所の [(ファイル, 行, 関数)] と、引いた数が上限に届いたか。"""
        try:
            found = json.loads(self.run("callers", name, "-f", rel, "--json", "-l", str(limit)) or "")
        except ValueError:
            return None
        rows, more = [], False
        for definition in found.get("definitions", []):
            if definition["definition"].get("filePath") != rel:
                continue
            more |= definition.get("total", 0) > limit
            sure = {edge["source"]: edge.get("line") for edge in definition.get("edges", [])
                    if (edge.get("metadata") or {}).get("confidence", 0) >= SURE}
            rows += [(caller["filePath"], sure[caller["id"]] or caller.get("startLine"), caller["name"])
                     for caller in definition.get("callers", []) if caller["id"] in sure]
        return rows, more


def edited_spans(text, tool, args):
    """編集した所の行の範囲（1 始まり）。編集より前の行は、編集の前後で同じ。"""
    pieces = [args] if tool == "Edit" else args.get("edits") or [] if tool == "MultiEdit" else []
    spans = []
    for piece in pieces:
        new, old = piece.get("new_string") or "", piece.get("old_string") or ""
        at = text.find(new) if new else -1  # 消しただけの所は、今のファイルに無い
        if at >= 0:
            start = text.count("\n", 0, at) + 1
            spans.append((start, start + max(new.count("\n"), old.count("\n"))))
    return spans


def touched(symbols, spans):
    """編集した所を含む記号と、編集した所で始まる記号のうちの関数。始まりの行で選ぶ。"""
    found = []
    for start, end in spans:
        before = [symbol for symbol in symbols if symbol[2] <= start]
        inside = [symbol for symbol in symbols if start < symbol[2] <= end]
        for symbol in ([max(before, key=lambda s: s[2])] if before else []) + inside:
            if symbol[1] in ("function", "method") and symbol not in found:
                found.append(symbol)
    return found


def connections(ws, rel, tool, args, shown):
    """編集した関数を呼ぶ所。一つの会話で、ファイルごと・関数ごとに一度だけ。"""
    if os.path.splitext(rel)[1].lower() not in CODE:
        return None
    graph = Graph(ws.root)
    users, symbols = graph.file(rel)
    if users is None:
        return None
    try:
        text = (ws.root / rel).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    lines = text.split("\n")

    def agrees(symbol):  # 索引の言う行に、今もその名前がある
        return 0 < symbol[2] <= len(lines) and symbol[0] in lines[symbol[2] - 1]

    spans = edited_spans(text, tool, args)
    edited = touched(symbols, spans)
    if edited and edited[0][2] <= spans[0][0] and not agrees(edited[0]):
        graph.run("sync", "-q")
        users, symbols = graph.file(rel)
        edited = [symbol for symbol in touched(symbols, spans) if agrees(symbol)]
    out = []
    if not shown.get(f"つながり:{rel}"):
        shown[f"つながり:{rel}"] = True
        count, names = users or (0, "")
        if count:
            out.append(f"- {rel} に頼るファイル {count}：{one_line(names, 200)}")
    for name, _, line in edited[:2]:
        if shown.get(f"つながり:{rel}#{name}"):
            continue
        shown[f"つながり:{rel}#{name}"] = True
        found = graph.callers(name, rel)
        if not found or not found[0]:
            continue
        rows, more = found
        where = "、".join(f"{path}:{at}（{caller}）" for path, at, caller in rows[:6])
        rest = f"、ほか {len(rows) - 6}" if len(rows) > 6 else ""
        out.append(f"- `{name}`（:{line}）を呼ぶ所 {len(rows)}{'以上' if more else ''}：{where}{rest}")
    if not out:
        return None
    return ("コードのつながり（codegraph の索引。番号で引くシステム呼出しや登録の表からの呼出しは出ない）：\n"
            + "\n".join(out))


def headline(text):
    """模型の版の一行：「説明」を含む見出しの下の最初の行。"""
    lines = text.splitlines()
    for index, line in enumerate(lines):
        if line.startswith("#") and "説明" in line:
            for body in lines[index + 1:]:
                if body.startswith("#"):
                    break
                if body.strip():
                    return shorten(re.sub(r"^\s*(?:[-*・]|\d+[.)．])\s*", "", body), 110)
    return None


def first_line(text):
    for line in text.splitlines():
        if line.strip() and not line.startswith("#"):
            return shorten(re.sub(r"^\s*[-*・]\s*", "", line), 90)
    return ""


def user_words(events, budget=4000):
    """ユーザーの原文。短い発言（訂正・指示）は全部そのまま、長いものは頭だけ。
    予算を越えたら途中を一行ずつの頭に縮め、最初の発言と最近の発言は残す。"""
    said = [(event["t"], event["text"].strip()) for event in events if event["k"] == "user"]
    if not said:
        return "# ユーザーの原文\n（記録なし。この会話はフックの記録より前に始まった）"
    full = [f"[{clock(when)}] {text if len(text) <= 400 else shorten(text, 600)}" for when, text in said]
    used, keep = len(full[0]), {0}
    for index in range(len(full) - 1, 0, -1):
        if used + len(full[index]) > budget:
            break
        keep.add(index)
        used += len(full[index])
    rows, stubs = [], 0
    for index, line in enumerate(full):
        if index in keep:
            rows.append(line)
        elif stubs < 20:
            stubs += 1
            rows.append(f"[{clock(said[index][0])}]（{len(said[index][1])} 字）{one_line(said[index][1], 80)}")
        elif rows and not rows[-1].startswith("（ほか"):
            rows.append("（ほか 0 件は記録にある）")
    hidden = len(full) - len(keep) - stubs
    if hidden > 0:
        rows = [row if not row.startswith("（ほか") else f"（ほか {hidden} 件は記録にある）" for row in rows]
    return "# ユーザーの原文（古い順。[ ] は時刻）\n" + "\n".join(rows)


def model_now(ws):
    text = ws.model_text()
    if not text:
        return f"# 模型\nまだ無い：{ws.model}"
    return f"# 模型（{ws.model}）\n{shorten(text, 2500)}"


def versions(events, limit=10):
    rows = []
    for event in events:
        line = headline(event["text"]) if event["k"] == "model" else None
        if line and (not rows or rows[-1][1] != line):
            rows.append((event["t"], line))
    if len(rows) < 2:
        return None, max(len(rows) - 1, 0)
    lines = [f"- {clock(when)} {line}" for when, line in rows]
    if len(lines) > limit:
        lines = lines[:1] + [f"- （途中の {len(lines) - limit} 版は記録にある）"] + lines[-(limit - 1):]
    return "# 模型の版の流れ（「説明」の一行目）\n" + "\n".join(lines), len(rows) - 1


def edits(events, hours=8):
    by_hour = {}
    for event in events:
        if event["k"] == "edit":
            hour = event["t"].astimezone().strftime("%m-%d %H時")
            by_hour.setdefault(hour, Counter())["/".join(event["path"].split("/")[-2:])] += 1
    if not by_hour:
        return None
    total = sum(sum(counts.values()) for counts in by_hour.values())
    lines = [f"この会話の編集 {total} 回。時間帯ごとに多く触ったファイル（新しい {min(hours, len(by_hour))} 時間帯）："]
    for hour, counts in list(by_hour.items())[-hours:]:
        top = counts.most_common(4)
        rest = sum(counts.values()) - sum(count for _, count in top)
        lines.append(f"- {hour}：" + "、".join(f"{path}×{count}" for path, count in top)
                     + (f" ほか {rest}" if rest else ""))
    return "# 編集の流れ\n" + "\n".join(lines)


def reports(events, limit=3):
    """自分がユーザーへ書いた最後の報告（Stop の記録）。圧縮の後に、何を伝えたかを思い出す。"""
    rows = [event for event in events if event["k"] == "stop"][-limit:]
    if not rows:
        return None
    return "# 自分の最後の報告（頭だけ）\n" + "\n".join(f"- {clock(event['t'])} {one_line(event['text'], 240)}"
                                                 for event in rows)


def repository(ws, start):
    lines = []
    if start:
        log = git(ws.root, "log", f"--since={start.isoformat()}", "-n", "10",
                  "--date=format-local:%m-%d %H:%M", "--format=%h %ad %s")
        if log and log.strip():
            lines.append("この会話の始まり以後のコミット（新しい順。並行する会話のものも含む）：")
            lines += ["- " + shorten(line, 120) for line in log.strip().splitlines()]
    status = git(ws.root, "status", "--porcelain")
    if status is not None:
        paths = [line[3:].split(" -> ")[-1].strip('"') for line in status.splitlines() if line.strip()]
        if paths:
            areas = Counter("/".join(path.split("/")[:2]) for path in paths)
            last = (git(ws.root, "log", "-1", "--format=%ct") or "").strip()
            age = since(datetime.datetime.fromtimestamp(int(last)).astimezone()) if last.isdigit() else "不明"
            lines.append(f"未コミットの変更 {len(paths)} ファイル（最後のコミットから {age}）："
                         + "、".join(f"{area} {count}" for area, count in areas.most_common(8)))
        else:
            lines.append("未コミットの変更は無い。")
    return "# リポジトリ\n" + "\n".join(lines) if lines else None


def reground(ws, state, source):
    events = ws.events()
    last_user = last_time(events, "user")
    last_measure = last_time(events, "measure")
    last_edit = last_time(events, "edit")
    starts = [datetime.datetime.fromisoformat(state["start"])] if state.get("start") else []
    start = min(starts + [event["t"] for event in events[:1]], default=None)
    flow, swaps = versions(events)
    head = (f"区切り（{ {'compact': '圧縮', 'fork': '分岐'}.get(source, '再開') }。"
            f"この会話の圧縮 {state.get('compactions', 0)} 回、ユーザーの最後の発言から {since(last_user)}、"
            f"最後に測って（ログ・撮影）から {since(last_measure) if last_measure else '一度も無い'}、"
            f"その間の編集 {count_after(events, 'edit', last_measure)} 件、"
            f"最後の編集から測る命令 {count_after(events, 'measure', last_edit)} 件、説明の入れ替わり {swaps} 回）。\n"
            "要約は直前の作業を一番詳しく残す。下はフックが事象から書いた記録で、起きたことはこちらにある。"
            "次の一手へ進む前に、ユーザーの原文と自分の流れ（版・編集）を並べ、同じ方向を向いているかを見る。"
            "向いていなければ、なぜそうなったかが今の説明の一番になる。")
    parts = [head, user_words(events), reports(events), model_now(ws), flow, edits(events), repository(ws, start)]
    return "\n\n".join(part for part in parts if part)


def opening(ws):
    lines = [f"作業の模型の置き場所：{ws.model}",
             "長い作業・性能・境界の設計・原因の調査では、mokei スキルに従ってここへ書く。"
             f"記録（ユーザーの原文・模型の版・編集・計測・報告）はフックが {ws.log.name} に書き、圧縮や再開の後に見せる。"]
    others = []
    for path in ws.folder.glob("*.md"):
        if path != ws.model:
            text = path.read_text(encoding="utf-8", errors="replace").strip()
            if text:
                others.append((path.stat().st_mtime, path, first_line(text)))
    if others:
        lines.append("この作業場の最近の模型（前の会話。続きの作業なら開く。中の事実は今のリポジトリと照らす）：")
        for mtime, path, line in sorted(others, reverse=True)[:3]:
            lines.append(f"- {clock(datetime.datetime.fromtimestamp(mtime))} {line}（{path.name}）")
    count = len(notes(ws.root))
    if count:
        lines.append(f"コードの覚え書き {count} 件（{NOTES.as_posix()}）は、対象のファイルに触れた時に出る。")
    return "\n".join(lines)


def milestone(ws, events, state):
    """ユーザーのいない時間が長いとき、模型を書かずに編集が重なったとき、一度だけ促す。"""
    out = []
    last_user = last_time(events, "user")
    nudges = [event for event in events if event["k"] == "nudge"]
    if last_user and now() - last_user >= QUIET:
        recent = [event["t"] for event in nudges if event.get("why") == "quiet" and event["t"] > last_user]
        if not recent or now() - max(recent) >= QUIET:
            last_measure = last_time(events, "measure")
            out.append(f"節目（ユーザーの最後の発言から {since(last_user)}、その間の編集 {count_after(events, 'edit', last_user)} 件、"
                       f"最後に測ってから {since(last_measure) if last_measure else '一度も無い'}）。"
                       "目的の一行と物差しの今の値を並べ、短い報告を一度書く（何を測り、何が動き、次に何をするか）。"
                       "物差しが動いていなければ、同じ方向の次の一手へ進まずに止まる。"
                       "自分が起動したもの（QEMU・ビルド・サブエージェント）を確かめる。")
            ws.record("nudge", why="quiet")
    last_model = last_time(events, "model")
    unmodeled = count_after(events, "edit", last_model)
    if unmodeled and unmodeled % MANY_EDITS == 0:
        marks = [event["t"] for event in nudges if event.get("why") == "model"]
        if not last_model or not marks or max(marks) < last_time(events, "edit"):
            out.append(f"模型を最後に書いてから編集 {unmodeled} 件。説明や次の一手が変わったなら、模型を今に合わせる"
                       f"（{ws.model.name}）。変わっていないなら何もしない。")
            ws.record("nudge", why="model")
    return "\n".join(out) or None


def only_measuring(ws):
    """直さずに測るばかりのとき、一度ずつ促す。測る回は欠陥を直さない。"""
    events = ws.events()
    measures = count_after(events, "measure", last_time(events, "edit"))
    if measures < MANY_MEASURES or measures % MANY_MEASURES:
        return None
    ws.record("nudge", why="measure")
    return (f"最後の編集から測る命令が {measures} 件、編集は無い。変える前の記録はもう取れているはず。"
            "測る・比べる・確かめ直す回は欠陥を直さない。症状の層が分かっているなら、ログや撮影を重ねず、"
            "コード（rg・codegraph で場所を絞り、原文を読む）と仕様の原文を読んで直す。直すまで測らない"
            "（AGENTS.md「確かめ方」）。")


def on_start(ws, data):
    source = data.get("source") or "startup"
    state = ws.state()
    state.setdefault("start", now().isoformat(timespec="seconds"))
    if source == "compact":
        state["compactions"] = state.get("compactions", 0) + 1
        ws.record("compact")
    ws.save(state)
    if source in ("compact", "resume", "fork") and (ws.model_text() or ws.log.is_file()):
        return reground(ws, state, source)
    return opening(ws)


def on_prompt(ws, data):
    text = data.get("prompt") or ""
    if text.strip() and not text.lstrip().startswith(MACHINE):
        ws.record("user", text=text)
    return None


def reads_only(command):
    """cd … && を外した先頭の語が、読むだけの命令か。"""
    head = re.sub(r"^\s*(cd\s+(\"[^\"]*\"|'[^']*'|\S+)\s*(&&|;)\s*)+", "", command).lstrip()
    return head.startswith(READ_ONLY)


def deny(ws, command, why):
    ws.record("blocked", text=one_line(command, 120))
    return {"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision": "deny",
                                   "permissionDecisionReason": "止めた：" + why
                                   + "（2026-10-29 ユーザーの指示。.claude/skills/nocturne-project/SKILL.md）"}}


def running_qemu():
    """動いている qemu-system の数。試験では MOKEI_QEMU_RUNNING で与える。"""
    override = os.environ.get("MOKEI_QEMU_RUNNING")
    if override is not None:
        return int(override)
    try:
        done = subprocess.run(["tasklist", "/FI", "IMAGENAME eq qemu-system-x86_64.exe", "/NH", "/FO", "CSV"],
                              capture_output=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return 0
    return sum(1 for line in done.stdout.decode("utf-8", "replace").splitlines() if line.startswith('"qemu-system'))


def images(root):
    """build の下の媒体（.img）。本線の nocturne.img と、直下か一段下の写し。"""
    build = root / "build"
    return sorted(set(build.glob("*.img")) | set(build.glob("*/*.img")))


def headless_tool(root, name):
    """試験の道具のスクリプトに、QEMU を窓なしで起こす印が残っているか。"""
    try:
        text = (root / "scripts" / name).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return bool(HEADLESS.search(text))


def on_pre_tool(ws, data):
    """禁じられた命令を実行の前に止め、理由を返す。"""
    if data.get("tool_name") not in SHELLS:
        return None
    command = (data.get("tool_input") or {}).get("command") or ""
    for pattern, why, skip_reads in FORBIDDEN:
        if skip_reads and reads_only(command):
            continue
        if pattern.search(command):
            return deny(ws, command, why)
    tool = TOOL_RUN.search(command)
    if tool:
        name = tool.group(1) + ".py"
        if headless_tool(ws.root, name):
            return deny(ws, command, f"scripts/{name} は今の作りだと QEMU を窓なしで起こす（-display none か CREATE_NO_WINDOW が"
                                     "残っている）。先に窓付きに直してから、対象を絞って走らせる。")
        if running_qemu() >= MAX_QEMU:
            return deny(ws, command, f"QEMU が既に {MAX_QEMU} 台動いていて、道具がもう一台起こす。先に一台を quit する。")
        others = [path.name for path in images(ws.root) if path.name != TOOL_IMAGE[name]]
        if len(others) >= MAX_IMAGES:
            return deny(ws, command, f"媒体は {MAX_IMAGES} つまで。build に既に " + "、".join(others)
                                     + " があり、道具の写しが三つ目になる。要らない写しを片づけてから。")
        return None
    if "qemu-system" in command and not reads_only(command) and running_qemu() >= MAX_QEMU:
        return deny(ws, command, f"QEMU は同時に {MAX_QEMU} 台まで。何台も起こすと RAM が尽きてページングに落ち、"
                                 "速さの値が歪む。先に一台を quit する。")
    if COPY_IMAGE.search(command):
        found = [path.name for path in images(ws.root)]
        if len(found) >= MAX_IMAGES:
            return deny(ws, command, f"媒体は {MAX_IMAGES} つまで。build に既に " + "、".join(found)
                                     + " がある。写しは一つまでで、要らない写しを片づけるか上書きで使い回す。")
    return None


def on_tool(ws, data):
    tool = data.get("tool_name")
    args = data.get("tool_input") or {}
    if tool in SHELLS:
        command = args.get("command") or ""
        if any(mark in command for mark in MEASURE):
            ws.record("measure", text=one_line(command, 120))
            return only_measuring(ws)
        return None
    path = args.get("file_path") or args.get("notebook_path")
    if not path:
        return None
    if tool in EDITS and ws.is_model(path):
        ws.record("model", text=ws.model_text())
        return None
    rel = ws.relative(path)
    if not rel:
        return None
    if tool in EDITS:
        ws.record("edit", path=rel)
    state = ws.state()
    segment = state.get("compactions", 0)
    shown = state.setdefault("shown", {}).setdefault(data.get("agent_id") or "main", {})
    parts = []
    full, brief = [], []
    for note in notes(ws.root):
        if not note.matches(rel):
            continue
        seen = shown.get(note.title)
        if seen is None:
            full.append(note)
        elif seen != segment:
            brief.append(note)
        shown[note.title] = segment
    if full:
        body = "\n\n".join(f"## {note.title}\n（{note.freshness(ws.root)}）\n{note.body}" for note in full)
        parts.append(f"コードの覚え書き（{NOTES.as_posix()}。{rel} に触れたので出す。"
                     f"今のコードと照らして使い、違っていたらその場で直すか消す）：\n\n{body}")
    if brief:
        parts.append("この会話で前に見せた覚え書きが " + rel + " に結び付いている："
                     + "、".join(f"「{note.title}」" for note in brief) + f"（本文は {NOTES.as_posix()}）")
    if tool in EDITS:
        parts.append(connections(ws, rel, tool, args, shown))
        parts.append(milestone(ws, ws.events(), state))
    parts = [part for part in parts if part]
    ws.save(state)
    return "\n\n".join(parts) or None


def on_stop(ws, data):
    """返事の記録と、言葉の確認。日本語の無い返事だけは書き直させる。"""
    text = (data.get("last_assistant_message") or "").strip()
    if not text:
        return None
    ws.record("stop", text=one_line(text, 400))
    if data.get("stop_hook_active"):
        return None
    prose = re.sub(r"```.*?```", " ", text, flags=re.S)
    prose = re.sub(r"`[^`\n]*`", " ", prose)
    prose = re.sub(r"\S*[/\\:.]\S*", " ", prose)  # パス・URL・識別子
    latin = len(re.findall(r"[A-Za-z]", prose))
    japanese = len(JA.findall(prose))
    if latin >= 80 and japanese < latin * 0.1:
        return {"decision": "block",
                "reason": "最後の返事に日本語がほとんど無い。ユーザーに見える文はすべて日本語で書く"
                          "（識別子・パス・コマンドはそのまま）。同じ内容を日本語で書き直して終える。"}
    return None


def main():
    data = json.loads(sys.stdin.buffer.read().decode("utf-8") or "{}")
    event = data.get("hook_event_name")
    handler = {"SessionStart": on_start, "UserPromptSubmit": on_prompt, "PreToolUse": on_pre_tool,
               "PostToolUse": on_tool, "Stop": on_stop}.get(event)
    if not handler:
        return
    ws = Workspace(data)
    try:
        out = handler(ws, data)
    except Exception:
        with open(ws.folder / "errors.log", "a", encoding="utf-8") as log:
            log.write(f"{now().isoformat(timespec='seconds')} {event}\n{traceback.format_exc()}\n")
        return
    if isinstance(out, dict):
        payload = out
    elif out:
        payload = {"hookSpecificOutput": {"hookEventName": event, "additionalContext": out}}
    else:
        return
    sys.stdout.buffer.write(json.dumps(payload, ensure_ascii=False).encode("utf-8"))


main()
