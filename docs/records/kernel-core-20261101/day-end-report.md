# 2026-11-01 の区切りの報告

main の先頭は `ea512555`。push していない。動いている書き手、監査役、QEMU は無い。

## 今日 main へ載せたもの

| コミット | 内容 | 報告 |
|---|---|---|
| `f2a489bd` | 共有 object | `sharedmm-report.md` |
| `d44c44b4` | CPU の扱い | `cpugeneral-report.md`、`audit-spec-cpu.md` |
| `12702764` | High Definition Audio | `hdaudio-report.md`、`audit-spec-hda.md` |
| `4db16e53` | 媒体の時計 | `mediaclock-report.md` |
| `ea512555` | USB と記憶の driver（監査の 48 件） | `usbstorage-fix-report.md`、`audit-usbstorage-fix*.md` |
| `47681cd5`〜`4974e870` | 設定の層の設計（作り直しが要る） | `settings-layer-design-2.md`、`audit-settings-design.md` |

## 確かめた範囲

`ea512555` の媒体を自分で見た。カーネルと媒体は警告なしで組めた。窓付きの QEMU を一台ずつ動かした。条件は q35、4 CPU、WHPX、旗なし。

| 回 | 見たもの | 結果 |
|---|---|---|
| NVMe、AC97＋none | baike の月球の動画、F12、Terminal、xHCI のキーボードと tablet | 下の時計の値。serial に panic と timed out の行は無い |
| AHCI、音の装置なし | Terminal、xHCI のキーボードと tablet、`echo hi > q` と `cat q` | `hi` が返った。`store: C: "Nocturne" on sata0`。serial に panic、timed out、failed の行は無い |

baike の月球の時計は、同じ頁で次の値である。

| 版 | 音の装置 | 範囲 | 時計／実時間 | 比 | drop | underrun |
|---|---|---|---|---|---|---|
| `d44c44b4` | なし | `@80.323..@154.626` | 68.472／74.303 s | 92.15% | 2 | 計数なし |
| `4db16e53` | AC97＋none | `@72.677..@215.125` | 142.441／142.448 s | 99.995% | 0 | 0/0 |
| `ea512555` | AC97＋none | `@97.039..@139.079` | 42.041／42.040 s | 100.002% | 0 | 0/0 |

- 実時間は guest の monotonic の時計で、ホストの時計と照らしていない。
- `d44c44b4` と後の二つは音の装置が違う。AC97＋none で同じ条件の比べは、書き手の値で `5043e3e6` 92.98%（差 5432 ms）、`6eef4835` 99.996%（差 13 ms、298 s）である。
- `ea512555` の `@123.060` の行に、生の音の差 -102.8 ms が一度出た。その行の濾した値は -0.32 ms、resync は 0 である。原因は調べていない。

## 確かめていない範囲

- 実機、Hyper-V。周波数の違う画面、VRR、3 枚の画面。
- 音を聞くこと。音の出口は `none` で、実機の HD Audio の輪は動かしていない。
- 参照のブラウザーとの部品の照合。スクロール、一時停止、シーク（`ea512555` の回の PgDn は F12 の入力欄に入り、頁は動いていない）。
- TEST SITES の組の全部。YouTube と Bilibili（`ea512555` では開いていない）。
- `ea512555` の USB の記憶装置からの起動。hub の抜き差し（書き手の回は一つ前の土台で見た）。
- ゲストの試験と WPT。性能の前の記録は無い。

## 線に届いていないもの

- 長いネイティブ処理の中で音が切れる（`innerHTML` の 11 s で 10478 ms）。音の供給が主スレッドの協調のままである。
- host の load が高い回で drop が 122、コマの差が 34 ms になった。根は分かっていない。
- 生の位置の報告は ±1 ms に入らない。濾した値は入る。

## 監査が残るもの

- 媒体の時計の差分の敵対的な監査（RFC 9110 の Range を含む）。
- USB と記憶の `usbhub_work` の末尾の 6 行。
- USB と記憶の根拠の文書のうち、SATA と ACS は手元に無い。SATA は販売物である。READ LOG EXT の道、DevSleep の SET FEATURES、`LINK_NS` と `READY_NS` はこの文書が要る。
- 共有 object、中核（APIC）、時間、合成と表示（EDID）。

## 未修理の欠陥

- HD Audio の監査の 41 指摘、`MAX_STREAMS` の 8。
- CPU の監査の F1〜F5。
- YouTube で browser が落ちる（`__JS_FreeValueRT`、`rt` の破損）。解析中の媒体の checkpoint を消すと 3 回中 0 回になった。仕組みは解いていない。
- YouTube の clock 約 48 s の `author-reset`。時計と音の層ではない。
- threadtest の二つの失敗、`dev/pci.c` の segment 0、NVIDIA の複数画面。
- `beep` の `-v`、mixed content の自動の格上げ、Range の readyState、HLS の同期の取得、動画の終わりの最後のコマ。

## ユーザーの答えを待つもの

- 設定の仲立ちをカーネルでなくシステムのプロセスに置く案。設計の §21 の 1 と 2 を置き換える。
- `settings-layer-design-2.md` の §21 の 3、5、6。
- `AGENTS.md` の文は、窓、システムのプロセス、カーネルを分けた形で次に出し直す。

## 次の手

1. 設定の層を 3 層で作り直し、敵対的に監査し、実装する。
2. 媒体の供給を主スレッドから外す。YouTube の落ちを解く。
3. HD Audio の 41 指摘と CPU の F1〜F5 を直し、照らす。
4. 残りの監査。NVIDIA の複数画面、threadtest、PCI の segment。
5. 地図と `kernel.md` を今の形にし、組の全部で一回り。
