# CSS 性能担当・第一報

## 範囲と実装

- `user/libc/web/css.c`、`webi.h`、`doc.c` の既存責務内に統合した。新しい製品 bridge は作っていない。
- 1 要素ごとの 4096 バイト arena に `style_t` を置く方式を廃止し、文書所有の 64 slot 単位共有 slab から固定本体を確保する。pseudo、backdrop、animation underlying を含む全 slot を要素の世代 chain で追跡する。
- 可変値用 arena は最初の可変値が必要になるまで未確保、初期 chunk は 512 バイト。大きい値は既存 allocator の必要量確保に従う。
- `style_previous_slots` と既存 `style_previous_mem` を、cascade の worker join と box borrow 更新・破棄の後で返却する。失敗／取消時も既存 retirement list から回収する。
- DOM の allocation owner と cascade 文書が異なる adoption を考慮し、閉じた文書の pool でも生存 slot がある間は保持する。最後の slot を返却してから orphan pool を解放する。
- inline style と WAAPI effect に immutable 宣言 AST を保持する。pointer identity だけではなく、所有する source bytes、長さ、文書 base URL を照合し、同じアドレスの内容変化・同内容の別アドレス・base 変更を処理する。
- custom-property の名前と値は computed tail に所有コピーし、宣言 cache 交換後も退役世代と子の継承 borrow が dangling pointer にならないようにした。
- native CSS keyframes は現行実装でも sheet 解析時の AST であり、毎 cascade に再解析される部分ではなかった。WAAPI の実際の sample 値が変わった場合は新しい宣言を解析するが、同じ sample で繰り返す cascade は AST を再利用する。

## 実行した確認

1. freestanding Nocturne target の clang `-fsyntax-only`：`css.c`、`doc.c`、`tests/styleincrementaltest.c`、成功。実 object link・OS 起動を証明する検査ではない。
2. `python -X utf8 tests/check_cssperformance.py`：製品の parser・pool・cache・retirement 関数を無変更抽出した実 C host 検査、22 検査／失敗 0。
   - 4096 node の plain fixed style は per-node arena 未確保。実測した `sizeof(style_t)` は 1360 バイト、退役の余裕 1 slab を含む 65 slab の固定本体保持量は 5,724,680 バイト。
   - 200 回の世代交換で slab 数が増えない。
   - join 前は旧本体を保持、join 後に返却。
   - 文書終了を跨ぐ adopted slot と、最後の orphan slot 回収。
   - inline AST 再利用、同じ pointer/長さで内容変更、同内容別 pointer、base 変更による URL 解決。
   - WAAPI 同 sample AST 保持、変更 sample AST 更新、effect 破棄。
3. `git diff --check`：成功。

## 失敗と訂正

- 最初の host direct translation-unit compile は Nocturne LP64 `ssize_t` と Windows CRT LLP64 の型衝突、続く link は未利用の exported CSS エントリの DOM／フォーム／thread シンボル未解決で成立しなかった。製品に stub を追加せず、既存 host 検査と同様に正確な製品関数を抽出する専用検査へ変更した。
- 最初の専用検査 compile はホスト側 `MIN`／`MAX` の宣言不足、補助側に同一マクロを追加して訂正した。最初の syntax 検査では `styleincrementaltest.c` の include path が不足しており、`-Iuser/libc/web` を追加して成立させた。
- 既存 `styleincrementaltest.c` の `chunk_size==4096` は旧設計への固定期待だった。新設計での slot 存在、plain 可変 tail 0、退役 slot 回収を要求する検査に変更した。合格のための弱化ではなく、この担当の目的そのものを強く確認する変更。

## 未確認と次

- 実 QEMU 画面、実サイト表示／入力／スクロール／性能比較、実媒体、Hyper-V はこの担当では実施していない。主担当の同条件検証が必要。
- slab は文書の最大同時保持量を再利用するため、その文書が生存している間の free capacity は保持する。node／tail／AST は既存寿命と更新単位で解放する。
- 親担当の独立 source review と、弱い仮定を再確認する同担当の第二報が次の作業。
