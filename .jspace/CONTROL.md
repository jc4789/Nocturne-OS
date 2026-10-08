# J-Space shared control state

Generated from control.json; use the CLI to update.

## Goal
> MLFQと描画/フォント性能の実装改善、Suttaメニューの画面内開閉とクリック例外修理、Discourse簡易表示の原因と対応を実サイトで確認し未完を明記する。Nocturne独自ABIと既存差分/許可フォントを保つ。

## Next
> scheduler/GPU現行ソースとnative animation cascadeを読んで実装を統合する

Level: high
Shared credits: 5 / 100

Solo limitation:
>

## Core

## Parked core

## Verified checkpoints
[]

## Questions
{}

## Agents

### Agent
> animation
Parent:
> root
Task:
> 実 PointerEvent と有限 animation
Owns:
> user/libc/web/js_animations.js, user/libc/web/js_pointer_events.js, user/libc/web/js_bootstrap.js
Active: True
Retirement: null

### Agent
> graphics
Parent:
> root
Task:
> 実描画性能
Owns:
> kernel/src/gui/wm.c, kernel/src/dev/gpu.c
Active: True
Retirement: null

### Agent
> menu
Parent:
> root
Task:
> 実 Sutta メニューの direction 修理
Owns:
> user/libc/web/js.c
Active: True
Retirement: null

### Agent
> mlfq
Parent:
> root
Task:
> 実 scheduler MLFQ
Owns:
> kernel/src/sys/sched.c, kernel/src/sys/sched.h, tests/mlfqtest.c
Active: True
Retirement: null

### Agent
> performance
Parent:
> root
Task:
> 実 face glyph cache 性能
Owns:
> user/libc/font.c
Active: True
Retirement: null

### Agent
> root
Parent:
> None
Task:
> MLFQと描画/フォント性能の実装改善、Suttaメニューの画面内開閉とクリック例外修理、Discourse簡易表示の原因と対応を実サイトで確認し未完を明記する。Nocturne独自ABIと既存差分/許可フォントを保つ。
Owns:
> .
Active: True
Retirement: null

## Repository map
{
  "areas": [
    {
      "owner": "mlfq",
      "path": "kernel/src/sys/sched.c",
      "purpose": "MLFQを実選択とsliceへ接続、BSP/AP安全境界保持",
      "tests": [
        "専用QEMUの既存scheduler/SMPチェックと対話CPU負荷"
      ]
    },
    {
      "coverage": "改善点は未確定",
      "path": "kernel/src/gui/wm.c",
      "purpose": "描画dirtyとframe経路。性能の具体的改善点を読む"
    },
    {
      "owner": "performance",
      "path": "user/libc/font.c",
      "purpose": "既存実face選択を保つ有限glyph解決キャッシュ",
      "tests": [
        "native fonttest245",
        "実サイトCanvas"
      ]
    },
    {
      "owner": "root/menu",
      "path": "user/libc/web/js.c",
      "purpose": "実directionと私有animation native hook"
    },
    {
      "owner": "root",
      "path": "user/libc/web/css.c",
      "purpose": "author属性を変えないanimation cascade"
    },
    {
      "owner": "animation",
      "path": "user/libc/web/js_animations.js",
      "purpose": "実時間の有限animation時計、実Material ripple"
    },
    {
      "owner": "root",
      "path": "user/libc/http.c",
      "purpose": "DiscourseのUA判定に影響するHTTP送信"
    },
    {
      "path": "scripts/platform_qa.py",
      "purpose": "個人data無しの最新媒体と実サイト検証"
    }
  ],
  "facts": [
    {
      "claim": "HTTPはNocturne/1.0を送る。UA別の公開応答ではscript0と24の差がある。",
      "evidence": "build/browser-menu-20261008/response-comparison.json",
      "receipt": {
        "path": "build/browser-menu-20261008/response-comparison.json",
        "sha256": "01c4c12423257be31667e4f385cbedbca3725523a25e6d6be33377c476704337"
      }
    },
    {
      "claim": "配信Material startPressAnimationはElement.animateを直接呼ぶ。",
      "evidence": "build/browser-menu-20261008/main-current.js",
      "receipt": {
        "path": "build/browser-menu-20261008/main-current.js",
        "sha256": "4b3d6030bdbd1f9ced9cc753c88ce372cbf5c0f44b4a62ebea1f31ac497b8786"
      }
    }
  ],
  "risks": [
    "kernel/AP共有allocatorとGUI制約を勝手に解放しない",
    "JS animationだけでは未対応CSS transformを完成したとはしない",
    "CPU/GPU/フォントの複合原因を一つと決めつけない"
  ],
  "summary": "新しいSuttaメニュー/クリック、Discourse応答、全OS応答性の限定改善。旧ブラウザー/フォント差分を保ち親が統合する。",
  "unknowns": [
    "GPU改善点と実行backend",
    "MLFQ実負荷の応答性",
    "Discourseの完全SPA起動は未確認"
  ]
}

## Security findings
