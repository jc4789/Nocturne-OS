# iframe の Fullscreen／PiP 修理（2026-10-12）

## 製品経路

- `web_presentation.c` は native frame の live／connected／文書所有を全祖先で検証する。最上位文書に実要求元の lease を保持し、Fullscreen は要求要素から最上位まで各文書の iframe を fullscreenElement とする。公開 `web_presentation_root` はこの検証後だけ native 最上位を返す。
- 応答の Permissions-Policy だけを文書生成時に複製する。祖先の応答制限、iframe `allow`／`allowfullscreen`、既存 sandbox を合成する。Fullscreen の既定は self、PiP は *。明示 `allow` が legacy 属性に優先し、重複宣言は後勝ち。`SB_ORIGIN` の opaque 文書は URL の同一 origin と扱わず、self/src を NULL 同士で許可しない。
- Fullscreen／PiP は同じ実媒体の native window に接続する。PiP 同一動画が Fullscreen の場合は既存 lease を退出する。最初の PiP と Fullscreen に native transient activation を要求・消費する。既存 PiP から別動画への切替で不要な activation 消費はしない。
- Fullscreen の viewport UA スタイルを祖先 iframe にも適用する。四辺 inset、transform、iframe border/padding を仕様に合わせる。作者の individual translate は規定にない上書きをしない。
- Shadow DOM 内の要求では native retarget predicate を CSS／UA と共用し、entry／exit に対応する shadow host だけを dirty にする。通常祖先や全 shadow root の再走査は追加しない。
- 各 native 遷移を文書所有 FIFO に保存し、実 JS task から trusted event を配送する。40ms 状態 polling を除去し、同一作者 task の enter→exit も両方保存する。native `events` は非消費 peek とし、wrapper の JS_EXCEPTION も検査する。private JS は全 Event／PiPWindow の実確保後、dispatch 直前の private `eventPop` で一件ずつ消費する。後続計画は private に保持し、native の残 FIFO が中断後の deadline を保つ。
- iframe 取外し・文書退役・Esc・native PiP close の既存退出経路を維持する。退役時は event allocation が失敗しても native window を解放し、root/leaf の残存 lease を切る。
- fullscreen の native 窓復帰に失敗した通常退出では文書 lease と host lease を共に保持する。退役 fallback 専用 mode 4 は復帰失敗でも host の文書 pointer を切り、解放済み文書を保持しない。
- 強制退役の clear も通常退出と同じ style/paint dirty を使い、存続する祖先 iframe に古い fullscreen UA スタイルを残さない。
- Fullscreen request/exit は native 成功で Promise を確定し、後続 task の fullscreenchange より先に microtask を処理する。0ms timer 後に同期 event dispatch する経路は除去した。
- PiP は実 native entry ごとの uint64 session を private BigInt に保持し、enter／leave／resize と Promise が同じ brand 付き Window を参照する。native entry 前に返却 tuple／Window を確保し、成功した新 request の Promise は enter event 後にその Window で resolve する。同 turn の exit で null や別 Window に変わらない。PiP 各 event は task を区切り、間に microtask checkpoint を置く。enter/leave の bubbles と policy 拒否の NotAllowedError も現在の W3C 手順へ合わせた。

## 確認と未確認

- target clang の native 構文確認を変更まとまり後に実施し、成功した。sandbox.h の既存 unused static 警告のみ。
- 追加の独立 review で native wrapper OOM、JS 側 Event 確保前の queue 消費、配列 index の表現限界、host/module 復帰失敗時の lease 不一致を実装修理した。native 三ファイルの target 構文と JS 構文は成功。最初の検査 command は Lexbor の include 指定不足で失敗し、正規 Makefile の二つの include を追加して訂正した。CRLF は保持し、差分確認は cr-at-eol を明示した。
- 最終 binding 生成、全媒体梱包、実 QEMU の iframe Fullscreen／PiP 操作・復帰・表示・入力・動画進行の受入は主担当が行う。構文確認を実表示成功へ置き換えない。
- PermissionsPolicy の全 feature introspection／report-only 配送を今回新設したわけではない。応答の対象は二つの実 presentation feature。HTTP(S) 以外の origin、完全な Structured Fields error recovery／任意の CSP host-source 文法は全面対応と報告しない。

## 一次仕様

- [WHATWG Fullscreen](https://fullscreen.spec.whatwg.org/)：文書／祖先 iframe の fullscreen、pending event、UA 描画。
- [W3C Picture-in-Picture](https://w3c.github.io/picture-in-picture/)：policy、媒体状態、activation、Fullscreen 退出、native window/event。
- [W3C Permissions Policy](https://www.w3.org/TR/permissions-policy-1/)：応答宣言、iframe container policy、既定 allowlist、文書生成時の inherited policy。
