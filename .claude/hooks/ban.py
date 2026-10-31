"""盤のフック：柵・鏡・言葉の確認（SessionStart・PreToolUse・Stop）。

- 柵（PreToolUse）：取り返しの付かない費用（RAM・ディスク・何時間もの待ち）を生む命令を、実行の前に止める。
  止めるものの一覧は .claude/skills/ban/SKILL.md「フックが止めるもの」。理由は何が壊れるかで書く。
- 鏡（SessionStart）：会話の始まりに盤の置き場所を一行。圧縮・再開の後は、盤のファイルがあればそのまま見せる。
  無ければ置き場所だけ。数えない、足さない。
- 言葉（Stop）：返事に日本語がほとんど無ければ書き直させる。

記録も状態も持たない。失敗は .claude/ban/errors.log に残し、会話には出さない。
"""
import datetime
import json
import os
import re
import subprocess
import sys
import traceback
from pathlib import Path

SHELLS = {"Bash", "PowerShell"}
AGENTS = {"Agent", "Task"}
LIST = "（一覧は .claude/skills/ban/SKILL.md「フックが止めるもの」）"
# 読むだけの命令。build の道の規則から除く。
READ_ONLY = ("grep", "rg ", "git ", "ls", "cat ", "cat.exe", "find ", "head", "tail", "sed -n", "wc ", "wc.exe",
             "stat ", "dir ", "Get-ChildItem", "Get-Content", "Select-String", "Test-Path", "Get-Item")
WORKTREE = ("作業木を手で作る命令。並べて書かせるサブエージェントは Agent の isolation: worktree で作業木を持つ"
            "（AGENTS.md「進め方」）。本体の会話は本体の作業木で働く。")
FORBIDDEN = (
    (re.compile(r"\bgit\b[^\n;&|]*\bworktree\s+add\b"), WORKTREE, False),
    (re.compile(r"qemu-system[^\n;&|]*(-display\s+(none|egl-headless)|-nographic)"),
     "QEMU を窓なしで起こす命令。黒画面でも分からず、何時間も待つことになる。QEMU は窓付きで二台まで（nocturne-project）。", False),
    (re.compile(r"python[^\n]*scripts[/\\](platform_qa|qtest)\.py"),
     "媒体を何枚も写し、窓なしの QEMU を何台も起こす道具。RAM とディスクを食い尽くす。試験は wpt.py（対象を絞る）と test.py。", False),
    (re.compile(r"build[/\\](qa|nocturne-platform)\b"),
     "build の下の記録のフォルダー。build には今のビルドの出力と試験の道具の出力（build/wpt）だけ。画像とログは会話の作業場（scratchpad）へ。", True),
    (re.compile(r"python[^\n]*scripts[/\\]build\.py(?:\s+(?:-j|--jobs)\s*\d+)?\s*(?:2>&1\s*)?(?:$|[|;&])", re.M),
     "目標の無いビルド。既定の all は vhd・vhdx・iso まで作って何分も取る。確かめは qemu-image、Hyper-V へ渡すときだけ all と書く。", False),
)
MAX_QEMU = 2
MAX_IMAGES = 2
TOOL_RUN = re.compile(r"python[^\n]*scripts[/\\](test|wpt)\.py")
TOOL_IMAGE = {"test.py": "test.img", "wpt.py": "wpt.img"}  # 道具が自分で作る写し
HEADLESS = re.compile(r'-display"[^\n]*"(none|egl-headless)"|CREATE_NO_WINDOW')
COPY_IMAGE = re.compile(r"(\bcp\b|\bcopy\b|Copy-Item|robocopy|qemu-img\s+convert)[^\n]*nocturne\.img")
JA = re.compile(r"[぀-ヿ一-鿿]")


def root_of(data):
    return Path(os.environ.get("CLAUDE_PROJECT_DIR") or data.get("cwd") or ".")


def folder_of(root):
    folder = root / ".claude" / "ban"
    folder.mkdir(parents=True, exist_ok=True)
    ignore = folder / ".gitignore"
    if not ignore.exists():
        ignore.write_bytes(b"*\n")
    return folder


def board_of(root, data):
    key = os.environ.get("CLAUDE_CODE_HOST_SESSION_ID") or data.get("session_id") or "unknown"
    return folder_of(root) / f"{key}.md"


# 鏡

def on_start(root, data):
    board = board_of(root, data)
    source = data.get("source") or "startup"
    if source in ("compact", "resume", "fork"):
        try:
            text = board.read_text(encoding="utf-8").strip()
        except OSError:
            text = ""
        if text:
            return f"# 盤（{board}）\n{text}"
    return f"盤：{board}"


# 柵

def reads_only(command):
    head = re.sub(r"^\s*(cd\s+(\"[^\"]*\"|'[^']*'|\S+)\s*(&&|;)\s*)+", "", command).lstrip()
    return head.startswith(READ_ONLY)


def deny(why):
    return {"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision": "deny",
                                   "permissionDecisionReason": "止めた：" + why + LIST}}


def running_qemu():
    override = os.environ.get("BAN_QEMU_RUNNING")
    if override is not None:
        return int(override)
    try:
        done = subprocess.run(["tasklist", "/FI", "IMAGENAME eq qemu-system-x86_64.exe", "/NH", "/FO", "CSV"],
                              capture_output=True, timeout=5)
    except (OSError, subprocess.TimeoutExpired):
        return 0
    return sum(1 for line in done.stdout.decode("utf-8", "replace").splitlines() if line.startswith('"qemu-system'))


def images(root):
    build = root / "build"
    return sorted(set(build.glob("*.img")) | set(build.glob("*/*.img")))


def headless_tool(root, name):
    try:
        text = (root / "scripts" / name).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return bool(HEADLESS.search(text))


def on_pre_tool(root, data):
    tool = data.get("tool_name")
    args = data.get("tool_input") or {}
    if tool == "EnterWorktree":
        return deny(WORKTREE)
    if tool not in SHELLS:
        return None
    command = args.get("command") or ""
    for pattern, why, skip_reads in FORBIDDEN:
        if skip_reads and reads_only(command):
            continue
        if pattern.search(command):
            return deny(why)
    run = TOOL_RUN.search(command)
    if run:
        name = run.group(1) + ".py"
        if headless_tool(root, name):
            return deny(f"scripts/{name} は今の作りだと QEMU を窓なしで起こす（-display none か CREATE_NO_WINDOW が残っている）。"
                        "先に窓付きに直してから、対象を絞って走らせる。")
        if running_qemu() >= MAX_QEMU:
            return deny(f"QEMU が既に {MAX_QEMU} 台動いていて、道具がもう一台起こす。三台目で RAM が尽きる。先に一台を quit する。")
        others = [path.name for path in images(root) if path.name != TOOL_IMAGE[name]]
        if len(others) >= MAX_IMAGES:
            return deny(f"媒体は {MAX_IMAGES} つまで。build に既に " + "、".join(others)
                        + " があり、道具の写しが三つ目になる。ディスクを食い、どれが今の媒体か分からなくなる。")
        return None
    if "qemu-system" in command and not reads_only(command) and running_qemu() >= MAX_QEMU:
        return deny(f"QEMU は同時に {MAX_QEMU} 台まで。三台目で RAM が尽きてページングに落ち、速さの値が歪む。先に一台を quit する。")
    if COPY_IMAGE.search(command):
        found = [path.name for path in images(root)]
        if len(found) >= MAX_IMAGES:
            return deny(f"媒体は {MAX_IMAGES} つまで（本線と写し一つ）。build に既に " + "、".join(found)
                        + " がある。三つ目はディスクを食い、どれが今の媒体か分からなくなる。要らない写しを上書きで使い回す。")
    return None


# 言葉

def on_stop(root, data):
    if data.get("stop_hook_active"):
        return None
    text = (data.get("last_assistant_message") or "").strip()
    if not text:
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
    handler = {"SessionStart": on_start, "PreToolUse": on_pre_tool, "Stop": on_stop}.get(event)
    if not handler:
        return
    root = root_of(data)
    try:
        out = handler(root, data)
    except Exception:
        with open(folder_of(root) / "errors.log", "a", encoding="utf-8") as log:
            log.write(f"{datetime.datetime.now().astimezone().isoformat(timespec='seconds')} {event}\n{traceback.format_exc()}\n")
        return
    if isinstance(out, dict):
        payload = out
    elif out:
        payload = {"hookSpecificOutput": {"hookEventName": event, "additionalContext": out}}
    else:
        return
    sys.stdout.buffer.write(json.dumps(payload, ensure_ascii=False).encode("utf-8"))


main()
