# 百度初期表示：共通資源経路の追加修理

ユーザーの約1分白画面・一斉表示の報告を改善済みとみなさず、主担当が独立1VM、WHPX／4CPU／4096MiB／1920×1080の比較前媒体を起動した。`build/nocturne-platform/compat-baike-perf-before-20261013/serial.log` はgzip修理前のbutton-format媒体の結果で、新sourceの証拠ではない。

原 `https://baike.baidu.com/` のHTMLは1681B、bodyにはroot divだけがあり、defer runtime5418Bとmain scriptの実行で本文を作る。現HTTP helperはgzipのincremental decoderを持ちながらAccept-Encodingを送っていなかった。原main scriptはidentity 4,117,334B、gzip 1,140,335Bで、展開後bytesは一致、SHA256は `86da8e055de5a0117aa758316ad0e3487bf2b45458682290cfb436387e8611c5`。取得原本は `build/compat-baike-home-20261013.html`、同 `.js`／`.js.gz`。

`user/apps/webfetch.c` のnetwork-owned outgoing headerへgzip交渉を接続した。[Fetch network steps 19–20](https://fetch.spec.whatwg.org/#http-network-or-cache-fetch)に従いRangeはidentity、明示native Accept-Encodingは保持。作者header分類の後で追加し、CORS preflight名へgzipを混入しない。未実装br/deflateを広告せず、UA・サイト・DOM例外を追加しない。

既存doc import walkは各rootの未完了import状態を返すようにし、linkのload/error producerをそのrootのcritical imports完了へ接続した。共有pending importも双方rootが待ち、要求はloading_idでdedupする。`dispatch_resource_event` の全stylesheet／全P_CSS joinを除去し、無関係sheetの通信が別rootのload listenerを止めないようにした。parser/deferのscript-blocking条件は別の仕様境界として保持した。

webnet debugに完了1回のqueue→complete／dispatch→complete期間を追加。queue時刻は既存、dispatchは既存pump時刻、追加clockはdebug完了のみ。期間にはTLS・pipe・GUI/native停止・helper reapが含まれ、ネット単独速度やworker CPU時間と呼ばない。default hotloopとworker／CPU数は変えていない。

## 確認と未確認

- 新outgoing関数6条件は成功：`build/compat-content-coding-fixed-20261013.log`。旧host抽出helperのmarker driftによる最初のValueError後、現sourceの関数境界へ訂正した。旧fixtureのorigin配列前提も現owned pointerへ整合。未変更旧群は反復していない。
- 製品incremental C gzip decoderへ実原資源を4096B chunkで入力し、4,117,334Bの元bytesと完全一致：`build/compat-baike-gzip-decoder-20261013.log`。
- 新native `csscachetest --owner-events` は共有import・無関係pending・別root errorの7条件で、まだ未実行。host構文・CRLF対応diff確認のみ成功。
- 比較前screen-0/1/2はLimine bootであり、百度白画面の証拠ではない。screen-3は1920×1080の百度本文／3枚hero未到着、screen-5は3枚hero到着を私自身も開いて見た。主担当の開始時計ではbrowser本文約13秒、hero完成約35秒。今回1VMでユーザーの約1分停止をそのまま再現したとはしない。
- 比較前serialのmain script compile327ms／execute181ms、最大native tick892ms。旧22秒native停止のログと別条件・別媒体として扱う。gzip・owner event修理後の同条件時間、実入力／上下スクロール、ユーザー環境での長停止解消は未確認。
