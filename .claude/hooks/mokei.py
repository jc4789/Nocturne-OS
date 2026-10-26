"""作業の模型を、会話の始まりに知らせ、圧縮や再開の後に読み込ませる（SessionStart フック）。

模型の書き方と使い方は .claude/skills/mokei/SKILL.md にある。
"""
import json
import os
import sys
from pathlib import Path


def context(data):
    session = data.get("session_id") or "unknown"
    root = Path(os.environ.get("CLAUDE_PROJECT_DIR") or data.get("cwd") or ".")
    folder = root / ".claude" / "mokei"
    folder.mkdir(parents=True, exist_ok=True)
    ignore = folder / ".gitignore"
    if not ignore.exists():
        ignore.write_bytes(b"*\n")
    path = folder / f"{session}.md"

    if data.get("source") not in ("compact", "resume"):
        return (f"作業の模型の置き場所：{path}\n"
                "長い作業・境界の設計・原因の調査では、mokei スキルに従ってここへ書く。")
    text = path.read_text(encoding="utf-8").strip() if path.is_file() else ""
    if not text:
        return (f"作業の模型はまだ無い：{path}\n"
                "要約から目的と今の説明を拾い、mokei スキルの書式でここへ書いてから続ける。")
    return (f"作業の模型（{path}）。要約の目録より先にこれを読み、"
            "「次の一手」が指すコードを読み直してから続ける。\n\n" + text)


def main():
    data = json.loads(sys.stdin.buffer.read().decode("utf-8") or "{}")
    out = {"hookSpecificOutput": {"hookEventName": "SessionStart",
                                  "additionalContext": context(data)}}
    sys.stdout.write(json.dumps(out))


main()
