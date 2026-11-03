# ブラウザー入力・要素・ストリーミングの実装記録（2026-10-08）

## 今回の実装

添付10画像を読み、新しい3担当で Forms、Parsing/Elements、Streaming を実装し、root が共有結線・メモリー所有・実OS統合を担当した。
Nocturne の独自 ABI、GUI、RAM root、`/data` 永続を維持。Unix/POSIX 互換層、fork、外部プレイヤー、DRM は追加していない。
これは実装済み進捗の引き渡しであり、全Web標準準拠・日常OS完成・全残件解消の宣言ではない。

### Forms

- checkbox indeterminate、クリック前状態と取消復元、native 描画・セレクター。
- label.control/live labels、fieldset.elements、datalist の実候補 UI、progress/meter の IDL・描画。
- image input 寸法、textarea.wrap/hard wrap/CRLF、Unicode first-strong dirname、autofocus。
- submitter の formAction/formEnctype/formMethod/formTarget と validation/cancel を実送信まで接続。
- URL encoded/text/plain/binary multipart、実 File/FileList、trusted native ファイル・ディレクトリ picker。
- opt-in 検索履歴：標準では無効。Hボタン/Ctrl+Shift+H の native 設定、明示有効化・無効化・消去。
  同一 origin/name のユーザー入力した search のみ。autocomplete=off、password を含む form、個人・認証情報候補を拒否。
  `/data/browser` の checksum 付き2世代 snapshot。自動送信もサイトへの履歴列挙 API も追加しない。

制限：実履歴の保存→再起動→候補選択は未確認。自然言語の秘密を完全判別できるとはしない。
accept picker 絞り込み、named target/別window POST、image submit 実座標、旧 Directory API は残る。
ファイル64件/16MiB、深さ32、picker512項目、履歴64件/field8件/値255bytesで有界。

### Parsing / Elements / Responsive

- Lexbor contextual fragment の insertAdjacentHTML、DOM forest、変更通知・resource 更新。
- native HTML serializer の void/raw text/namespace/attribute escaping/NBSP/doctype/PI、fragment U+FEFF保持。
- hidden、picture/source の実 MIME/media/srcset/sizes 選択と resize/resource 更新。
- native modal dialog の top-layer/::backdrop/:modal、外側 inert、focus/Tab制限・復元、trusted Escape cancel/close。
- method=dialog は native validation/submit取消後に owner form の近傍 dialog を閉じる。HTTP GETへ誤fallbackしない。

制限：GitHub実ページに既存dialogがなく、modal/Tab/Escape/method=dialog の製品操作受入れは未確認。
ping、until-found、full namespace prefix、sizes calc/em/auto、light-dismiss/CloseWatcher等は残る。
現行 WHATWG の PI node を旧HTML5test tokenizer probeのために commentへ偽装しない。

### Streaming / メディア / メモリー修理

- clear HLS VOD（TS/fMP4）・static single-period DASH、持続decoder、2-track、seek/GOP preroll。
- FFmpeg TS demux closure を出自manifest付きで導入。AAC-LC/HE-AAC/SBR/PSの実decoderへ結線。
- MSE の sampleごとの codec configuration、合法init反復、実decoder切替、appendのowned input移譲。
- CPU bounded-yield と実入力不足を区別。APPENDだけで不要seek/pending破棄を起こさない。
- remove時にcodecをclose/reopenし、高水位FramePoolを実解放。provider/cursor/metadata/trimは保持。
- ENOMEMをquotaとして伝え、malformed media/fatal EOSに誤分類しない。古outputのコピー増量も削除。
- TextTrack/VTTCue/activeCues/events/WebVTT private fetch、native plaintext caption を実描画へ接続。
- Rangeの strong validator/表現同一性、validatorなし小mediaの immutable full-body fallback。
- 実 Readable/Writable/Transform/BYOB/tee、Response.body/clone/bodyUsed、Blob.stream。
- Fetch→XHR の重複bodyコピーを除去。private slot/WeakMap intrinsicを固定し、page getterへbacking bytesを漏らさない。
- Canvas path8192点、heap課金scratch・有界fill/stroke、非再帰sort、readback単一所有、putのbuffer借用。
- native大payloadとQuickJS ArrayBuffer backing allocationの直前に、quota不足なら不要cycleを回収する機会を追加。
  allocator内GC・同期author finalizer・生存buffer破棄はしない。

現在の上限：MSE子64MiB、parent aggregate96MiB、encoded32MiB、JS128MiB、HTTP buffered16MiB。
後続OOM修理では上限をさらに上げていない。rollback原文 staging と padded demux packets の同時保持は容量境界として残る。
encrypted/live/dynamic/discontinuity、full MSE/WebM reinit、full WebVTT layout/region/style、incremental HTTP/stream uploadは未完成。
JS Streamsの存在をネットワークの逐次配信完成と混同しない。

## 実OS・実サイトでの結果

QEMU/WHPX、4 vCPU、2GiB、隔離boot/dataを使用。既存ユーザーデータは変更しない。
内蔵JS consoleから既存実DOM・フォーム・プレイヤーを操作。自作Web互換ページやWikipediaは試験に使用しない。

| 実サイト | 確認できたことと残る失敗 |
|---|---|
| HTML5test | 添付254→286/555、Forms54→64/65、Elements23→26/30、Responsive15/15、Canvas18/25、Video33、Streaming5。12画面を最後まで確認。Parsing2/5、Streams4/6。旧 WriteableStream typo等のため存在stubを追加しない |
| DuckDuckGo | 実入力→requestSubmit→`?q=Nocturne+OS&ia=web`の検索結果表示。sendBeacon等は未完成 |
| Baidu Baike | 中国語ホーム、画像、入力、実HTTP Body clone/read。Axios404/unhandled例外は残る |
| Amazon | 実ホーム、picture/form/実Fetch clone。動画の緑色破損は未修理、正常再生とは扱わない |
| OpenAI指定記事 | 実アクセスしたがCloudflare/iframe/JSメモリー境界で記事表示未達。回避していない |
| DASH-IF公式 | 12.43→32.43→57.43→77.43秒、480x270の実BBB frame。HE-AAC実音声track、録音PCM非ゼロ。旧Chart path RangeErrorなし。聴音/full AV同期は未確認 |
| hls.js公式 | 実時刻 12.497, 27.514, 40.769, 0 秒。fatal停止が残り、安定再生未達。 前回実画像でBBB動画＋日本語plaintext字幕描画を確認。全auto-quality/長時間再生の合格とはしない |
| GitHub | openai-python実repositoryの画像/表/リンクを描画。既存dialogが0件のためmodal受入れ証拠にはしない |

## ビルド・証拠の場所

- 最終製品build：`build/web-input-20261008/build-buffer-gc.log`、終了0。
- HTML5test全一覧：`build/nocturne-platform/webinput-html5-integrated-01/page-0.png`～`page-11.png`。
- DASH実動画/ログ：`build/nocturne-platform/webinput-dash-integrated-01/screen-2.png` / `serial.log`。
- HLS最新：`build/nocturne-platform/webinput-hls-buffer-gc-01/serial.log`、`screen-*.png`。
- 実字幕画像：`build/nocturne-platform/webinput-hls-reclaim-01/screen-1.png`。
- 録音の測定：`build/web-input-20261008/current-audio-evidence.json`。元WAVは書換えず、unfinished QEMU headerも記録。
- 最新source hash：`build/web-input-20261008/current-source-sha256.json`。
- 各担当の報告・候補は `build/web-input-20261008/forms/`、`html_elements/`、`streaming/` に保持。

全サイトを同一最終imageで再走査したとは主張しない。DASH/HTML5testはその直前の統合image、GitHubはmethod=dialog統合後、最終GC/decoder保持修理後の再確認はHLS。
古い停止ログ・ゼロPCM・bootstrap失敗等は履歴であり、最新成功や最新失敗へ混同しない。

## 残る実装優先順位

1. HLS高bitrateのtransactional staging＋padded packetのpeak所有を、安全なrollbackを保って減らす。
2. Amazon破損frame、実音声/AV同期、native modalと検索履歴の製品操作受入れ。
3. ping、picker/target/画像submit座標、incremental HTTP、残るWebVTT/Canvas機能。
未解決を『完了』に変更したり、APIの存在だけで全機能を合格扱いにしない。
