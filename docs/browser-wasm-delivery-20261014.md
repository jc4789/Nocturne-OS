# ブラウザー WebAssembly・wasm2c/TCC・JSPI 引渡し記録

## 実装と証拠の範囲

WABT 1.0.42（Apache-2.0）の検証器/Storeを保全し、wasm2c host-resource生成C→ゲストTCC→ブラウザー内native機械語へ接続した。共有正本のMemory/Table/Global、import再入、参照GC、コード寿命、取消/OOM/stack guard、同threadの本物JSPIを統合。CLI `/bin/wasm2c`、C runtime/sysroot、licensesも同梱。サイト特例、Launch強制、空Atomics、固定crossOriginIsolatedはない。Emscripten checkoutは不要で変更していない。ユーザーの開始時AGENTS.md差分は保全した。

最新全体構築 `build/wasm-full-build-jspi-first.log` は終了0。`build/wasm-jspi-first-20261014/test-serial.log` は15群成功/失敗0/skip0、基本170・native境界41・WindowJSPI74・HTTPWorker50・CSP・Fetch297・XHR89・DOM/Worker/native threadを含む。追加Worker JSPI12は `build/wasm-jspi-worker-final-20261014/test-serial.log` でWorker合計62成功。Puter同VMのTCCguard29/0・GC256・native/fallback中断GC・退役再入も成功。CLIはguest wasm2c→guestTCC→実ELF結果42。

## 指定4サイトの最新媒体での実受入

WHPX、4096MiB、4CPU、1920×1080、最大化ブラウザー＋console。ホスト空きRAMのため独立VMは2台ずつ並行。主担当自身が下記PNGを開いて確認した。場所は `build/nocturne-platform/<run>/`。各 `serial.log` にconsole原文、`console-evidence.log` 等に抽出を保全した。観測補助は読取り専用、操作はnativeキー/マウス。

| run | 見た画像・実操作 | 判断と残失敗 |
|---|---|---|
| `wasm-jspi-baike-20261014` | `current-console.png`, `down-console.png`, `up-console.png`, `search-console.png`, `history-back-console.png`, `history-forward-console.png`。初回から中国語本文/画像、PageDown/Up、検索WebAssembly→実曖昧語ページ、Alt-left/right。 | 表示・これらの操作成立、Reload援助なし。履歴戻り直後は画像再読込途中、AxiosError Network Errorは残る。 |
| `wasm-jspi-youtube-20261014` | `current-console-2.png`, `paused-console.png`, `resumed-seek-console.png`, `down-console.png`, `up-console.png`, `seek-controls-console.png`, `timeline-seek-console.png`, `seek-complete-console.png`。映像進行、実click停止47.014秒保持→再開、上下scroll、実timeline click→seeked365.914→391.947秒。 | 一部操作成立。しかし207.146秒付近にvideo lookahead allocationで一旦停止、seek/画質854×480への切替後再開。字幕重なり、observer例外も残る。無停止/無回帰ではない。 |
| `wasm-jspi-cnn-20261014` | `loaded-console.png`, `article-console.png`, `down-console.png`, `home-down-console.png`, `home-up-console.png`, `home-up-painted-console.png`。H2実target確認→実click→指定記事/動画URL、Alt-left、PageDown/Upで下段記事/写真→先頭。 | ホーム本文/写真と履歴・上下scroll成立。動画先は黒画面未達、ホーム上部にも大黒領域。FAVE TypeError/例外と重いstyle/layoutが残る。menuは旧試験未達、今回成功へ変更しない。 |
| `wasm-jspi-puter-first-20261014` | `before-launch-console.png`, `after-launch-console.png`, `down-console.png`, `up-console.png`。JSPI警告消失、Launch有効・start-btn実target→native click。 | loader54394Bは初期native5→Launch後累積2modules/108788B/native10calls/fallback0。JSPI使用0。Atomics自体undefinedでwaitAsync読出し失敗、Retry表示。Firefox UI/提供参照画像は未達、Retry未操作。 |

4runとも撮影解像度一致・harness正常終了。ただしharness acceptedは画面全面合格ではない。YouTube音声は実PCM供給/録音あり、未聴取で音質・映像音声同期は未確認。最新動画確保失敗は `avmedia.c:439/446` の画像malloc/realloc失敗まで絞った。実media quotaはSIZE_MAX、関連媒体sourceに今回差分なしだが、失敗要求量/allocator状態がなくOOM/断片化/間接圧の因果は未確定。既存とも今回回帰とも断定しない。

## 過去段階と未対応を混同しない

旧interpreter/CSP段階の11群・6群、旧三サイト操作、native初回14群の結果と失敗も各ログ/第一報へ保全。旧GC helper本体は成功したが、誤ready markerと1280×800撮影によりハーネスは失敗した。初回のMemory.PageSize/Workerとdata Fetchの失敗は製品修理後の別検証で確認した。旧百度百科のReload援助は最新初回表示成功と分離する。全Core3.0/GC/typed refs/EH/JS文字列、shared/SAB/Atomics、Module cloneは全面未対応。buffered streamingを並列compile高速化と呼ばない。全OOM/任意host stack exhaustion/同条件性能比較/Hyper-V実機は未実証。

## 配布と終了状態

`build/wasm-final-artifacts-jspi.log` がinitrd16payload、IMG内kernel/initrd、VHD/VHDX論理内容、ISO extent、関連source freshnessの一致を記録。最新配布物は `build/nocturne.img`, `build/nocturne.vhd`, `build/nocturne.vhdx`, `build/nocturne.iso`。Hyper-V VM/使用中媒体/永続dataは操作していない。

この作業所有のQEMUは全停止。最新/旧検証用scratch28files・7717519360bytesは絶対path/非reparse/全QEMU停止を確認してrmで回収、D空き81756971008bytes。前段34files・8254390272bytesの回収も別logに保存。通常 `build/data.img`、4配布媒体、source、ログ/PNG/metadataは保全。削除済scratchをそのまま再起動可能とは扱わない。確認記録は `build/wasm-final-scratch-check.log` と `build/wasm-final-scratch-cleanup.log`。

## 依頼との照合

wasm2cの単なる同梱でなくブラウザーnative実行、JSPIの実中断再開、subagent3名による責務実装/反証、指定4重サイトのmaximized/console実観測、console取得、IMG/VHD/VHDX/ISO更新を実施した。Firefox本体起動とサイト全面互換、性能無回帰は未達または未証明であり、完了へ偽装しない。詳細な独立照合は `docs/browser-wasm-root-review-20261014.md`。
