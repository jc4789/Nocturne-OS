# 性能変更に関連する WPT 原本の限定接続（第一報）

## 範囲と原本

- 主担当が実サイト／実 GUI を受け入れる作業とは別の補助確認。WPT 全体は実行しない。製品ソースと提供 checkout は変更していない。
- 原本 root: `D:/Programes/cloned repos/wpt`。HEAD: `cd1d8aa5bfebc15d1af2af230c7ff95f9e582a4c`。下記五ファイルの `git status --short -- ...` は空だった。
- 最大三つの testharness HTML（testdriver 不要）だけを選択した。各原 assert／期待／CSS／JS を複製・差替え・除外しない。

| checkout 相対パス／HTTP path | 関連境界 | 原ケース数 | bytes | SHA256 |
|---|---|---:|---:|---|
| `css/cssom-view/elementFromPoint-negative-z-index-inline-overflow-clip.html` | 負 z-index と inline overflow clip を含む hit 順・clip | 1 | 1554 | `41fb3ead95fe420015748927ada82c3ab17a797642ed4004d106abd2d1645cb1` |
| `css/cssom-view/elementFromPoint-dynamic-anon-box.html` | 動的 visibility による匿名 scrollbox の hit 失効 | 1 | 1310 | `e3ad147b69f8d5423f67fdb434e93d4e5412ef4a0b027f7d7c6ef3c28ef5376a` |
| `css/cssom-view/scrollWidthHeight-negative-margin-001.html` | visible／hidden／auto／clip 各 overflow と負 margin の scroll extent | 4 | 1344 | `3f915124cacc59c49d67d09cb63dcaf4e92ad464aac8e56528fd7912c85f32bb` |
| `resources/testharness.js` | 原 assert／結果・完了通知の提供 | — | 198420 | `7a108d7cfa98ac413ddb38f82fd509fb2037dfcc25ccf466567520cbb6bdf1fa` |
| `resources/testharnessreport.js` | 原 vendor integration script | — | 1231 | `3777176fc736b0a78cf7617b9613b220f635daae7536ec7ebb99c61dbb525aae` |

未実行の第三候補 `scroll-clamp-after-transform-shrinks-overflow.html` は transitionend／transform にも依存するため、今回の一般 extent 修理に近い第三行へ **実行前に** 選び直した。FAIL を見て除外したものではない。

## 接続した runner

`D:/Programes/llm vibe slop os/tests/wptperformancetest.c` は既存 `web_live_response`／`webnet`／`web_tick`／`web_layout`／`web_paint` と Nocturne の `/bin/webfetch` を使う限定 native runner。全 suite discovery、専用 adapter、偽 network response、assert の移植はない。

- 原本 HTML と二つの script は HTTP 受信 bytes の SHA256・長さを実行前に照合する。不一致は実行基盤失敗として残し、変わった fixture を実行しない。
- HTML parser 初回 tick 前に `window` の capture `load` listener を登録する。原 `testharness.js` の script load 後、次の inline test より前に標準 `add_completion_callback` へ読み取り専用 observer を接続する。DOM／style／fixture関数／harness関数を置換しない。
- 原 `add_completion_callback` は完了後に登録しても再通知しないため、単純な「あとから登録」で成功を装わない。登録 marker がない、完了通知がない、原ケース数 1／1／4 が揃わない場合も失敗にする。
- 原ケースの name／status／message／stack、および harness status を JSON のまま出力する。nonzero status は FAIL／TIMEOUT／NOTRUN／PRECONDITION_FAILED のいずれも非成功。通信・例外・runner不備は別の実行基盤失敗数で記録し、元の原結果を隠さない。
- viewport は 1024×768。三ページ各 30 秒の外側待機期限と native JS task deadline を設定する。原 harness の timeout 設定、fixture timer、CSS、assert は変更しない。ページごとに文書・通信 child・response・canvas を解放する。

## 主担当への限定実施条件

主担当だけが標準 HTTP server と専用 guest を起動する。既存永続 data は使わない。標準 server の root は checkout 自体であり、コピー／改変した fixture tree ではない。

```text
python -X utf8 -m http.server 8766 --bind 0.0.0.0 --directory "D:/Programes/cloned repos/wpt"
```

guest では現行 sysroot／libc／webfetch と当 runner を使い、既存 tcc 経路で接続する。TinyCC が標準で BearSSL header／library を含むことは `scripts/mksysroot.sh` の既存配置で確認した。

```text
tcc -o /tmp/wptperformancetest /tests/wptperformancetest.c && /tmp/wptperformancetest http://10.0.2.2:8766/
```

`tests/runtests.c` group 登録は主担当所有。成功時の集計行は `限定WPT 3 ページ 6 原ケース 0 失敗 0 ページ失敗`。ただしこの文字列だけでなく、各原ケース JSON・harness JSON・受信 hash・各ページ結果も読む。失敗は期待を変更して通さず保存する。3 URL は表の HTML path へ `http://10.0.2.2:8766/` を前置したものだけ。

## 実施済みと未確認

- 元ファイルの read-only 内容／git／hash 確認、および runner の native clang `-fsyntax-only`、owned diff check を実施。正規 native target／include／warning flags で exit0、warningなし。
- 初回 syntax command は `-nostdinc` で clang builtin `stddef.h` を落とす検査設定不備により失敗。`Makefile` の native target／freestanding 方針へ訂正し、原期待や製品は変更していない。
- WPT 実行結果、Nocturne guest compile／動作、実画面、性能比較は **未確認**。担当は HTTP server／guest／全build を起動していない。主担当の指定した一回の限定 guest へ引き渡す。
