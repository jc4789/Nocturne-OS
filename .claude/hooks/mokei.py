"""作業の模型のフック。

会話の始まりに置き場所を知らせ、ユーザーの発言の時刻を控え、圧縮や再開の後には区切りとして
目的と物差しを見せる。圧縮をまたぐと時間と回数の感覚が消えるので、それをここで数えて渡す。
模型の書き方と使い方は .claude/skills/mokei/SKILL.md にある。
"""
import datetime
import hashlib
import json
import os
import re
import sys
from pathlib import Path


def now():
    return datetime.datetime.now(datetime.timezone.utc)


def paths(data):
    session = data.get("session_id") or "unknown"
    root = Path(os.environ.get("CLAUDE_PROJECT_DIR") or data.get("cwd") or ".")
    folder = root / ".claude" / "mokei"
    folder.mkdir(parents=True, exist_ok=True)
    ignore = folder / ".gitignore"
    if not ignore.exists():
        ignore.write_bytes(b"*\n")
    return folder / f"{session}.md", folder / f"{session}.state.json"


def load(state_path):
    try:
        return json.loads(state_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def save(state_path, state):
    state_path.write_bytes(json.dumps(state, ensure_ascii=False).encode("utf-8"))


def section(text, name):
    m = re.search(rf"^# {name}[^\n]*\n(.*?)(?=^# |\Z)", text, re.M | re.S)
    return m.group(1).strip() if m else None


def moment(iso):
    return datetime.datetime.fromisoformat(iso) if iso else None


def since(when):
    if not when:
        return "不明"
    minutes = int((now() - when).total_seconds() // 60)
    return f"{minutes // 60} 時間 {minutes % 60} 分"


# アプリや道具やほかの担当が会話へ送る知らせ。ユーザーの発言ではない。
MACHINE = ("<task-notification", "<side-sessions-event", "<ci-monitor-event",
           "<system-reminder", "<local-command-stdout", "<local-command-stderr", "<local-command-caveat",
           "[SYSTEM NOTIFICATION", "Another Claude session sent a message")


def prompt(data):
    """ユーザーの発言の時刻を控える。"""
    text = (data.get("prompt") or "").lstrip()
    if text.startswith(MACHINE):
        return None
    _, state_path = paths(data)
    state = load(state_path)
    state["last_user"] = now().isoformat()
    save(state_path, state)
    return None


def start(data):
    model_path, state_path = paths(data)
    source = data.get("source")
    if source not in ("compact", "resume"):
        return (f"作業の模型の置き場所：{model_path}\n"
                "長い作業・性能の改善・境界の設計・原因の調査では、mokei スキルに従ってここへ書く。")

    state = load(state_path)
    text = model_path.read_text(encoding="utf-8").strip() if model_path.is_file() else ""
    goal, ruler = section(text, "目的"), section(text, "物差し")
    digest = hashlib.sha1((ruler or "").encode("utf-8")).hexdigest()
    written = (datetime.datetime.fromtimestamp(model_path.stat().st_mtime, datetime.timezone.utc)
               if text else None)
    last_compact = moment(state.get("last_compact"))
    stale = bool(text and last_compact and written < last_compact)
    if source == "compact":
        state["compactions"] = state.get("compactions", 0) + 1
        state["ruler_same"] = state.get("ruler_same", 0) + 1 if digest == state.get("ruler") else 0
        state["last_compact"] = now().isoformat()
    state["ruler"] = digest
    save(state_path, state)

    facts = (f"区切り（{'圧縮' if source == 'compact' else '再開'}。この会話の圧縮は {state.get('compactions', 0)} 回）。"
             f"ユーザーの最後の発言から {since(moment(state.get('last_user')))}。")
    if stale:
        facts += f"模型は前の区切りの後、書き換えられていない（最後の書き換えから {since(written)}）。要約の方が新しい。"
    if not text:
        return (facts + f"\n作業の模型はまだ無い：{model_path}\n"
                "要約から目的を拾い、目的が叶ったときユーザーが何を見て分かるか（物差し）を書いてから進む。")
    if ruler is None:
        facts += "模型に「物差し」の欄が無い。目的が叶ったらユーザーが何を見て分かるかを、先に物差しとして書く。"
    elif state["ruler_same"]:
        facts += f"模型の「物差し」は直近 {state['ruler_same']} 回の区切りの間、書き換えられていない。"
    return (facts + "\n"
            "圧縮は続きの合図ではなく区切り。続ける前に問う：前の区切りから入れた改善で、物差しは目的の方へ動いたか。"
            "測っていない、または動いていないなら、同じ方向の次の一手へ進まず、測るか、なぜ効かないかを今の説明の一番にするか、"
            "今の値と分かっていることをユーザーに伝えて止まる。"
            f"次の一手と残りの欄は模型ファイル（{model_path}）にある。\n\n"
            f"# 目的\n{goal or '（欄が無い）'}\n# 物差し\n{ruler or '（欄が無い）'}")


def main():
    data = json.loads(sys.stdin.buffer.read().decode("utf-8") or "{}")
    event = data.get("hook_event_name") or "SessionStart"
    context = prompt(data) if event == "UserPromptSubmit" else start(data)
    if context:
        out = {"hookSpecificOutput": {"hookEventName": event, "additionalContext": context}}
        sys.stdout.write(json.dumps(out))


main()
