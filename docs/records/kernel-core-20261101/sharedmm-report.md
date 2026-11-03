# sharedmm の報告

## 依頼文の全文

> あなたの名は「sharedmm」。まず `D:\Programes\llm vibe slop os\docs\kernel-core-20261101\common-brief.md` を読み、その全部に従う。ただし土台は main の 5043e3e6（中核・USB・時間の ABI・合成と表示・I/O が載った形）。自分の作業木が古ければ、まず main を取り込む。QEMU のポートは HMP 45476・QMP 45477。材料は `docs/records/kernel-core-20261101/iopath-report.md` の「残った欠陥」、`audit-kernel.md` の要点 4、`compositor-report.md`。
>
> # 仕事：共有メモリーと program の text の道を、大域の錠から出して mm の持ち物にする
> `kernel/src/sys/syscall.c` の約 1500 行に共有 object の実装が残っている。共有メモリーと text の fault は大域の spinlock `shared_lock` を割り込みを止めて握り、錠の中で frame を確保・0 埋め・map し、revoke は `shared_all` を歩く。窓の面が共有 object になった（`SYS_WIN_SURFACE`、`shared_kernel_new`・`shared_kernel_end`・`shared_kernel_handle`）ので、この錠は合成の締切の道に載っている。
>
> 必ず入れる形：
> - 共有 object を `kernel/src/mm/` の部品にする（責務：frame の持ち方、写像の一覧、fault、revoke）。syscall.c には引数の検査と受け口だけを残す。
> - object ごとの錠。compose（部分の object）どうしと scope の木の順を設計して `arch/spin.h` の rank で書く。fault は錠の下で 0 埋めをしない（frame の用意は錠の外、載せるときに競り負けたら返す）。割り込みを止めない。
> - revoke は全 object を歩かない（持ち主から届く形）。
> - `demand_fill` が割り込みを止めた空間の錠の下で最大 16 個の frame を 0 で埋める件（読み込みの窓で kernel の標本が一番多い）も同じ形で直す。前の値：読み込みの窓の demand_fill の標本 433、147（iopath の報告）。
> - `shared_scanout_open`（`dev/pciuser.c` から呼ばれる画面の道）、`vmm` の `PTE_SHARED`、GPU の driver の道を壊さない。
>
> 範囲の外：設定の層（別の書き手）。
>
> 確かめ：組み（`kernel`・`qemu-image`、変数なし）。QEMU で、デスクトップ、窓を開く・大きさを変える・閉じる、3D Cube、ブラウザーで Wikipedia を読み込んでスクロール、Terminal の clang で小さなプログラム。`prof:` の行の読み込みの窓の標本を前の値と並べる。ゲストの試験のうち共有メモリーに触れるもの（`tests/nativeabi` など）を `scripts/test.py` で回す。フックが媒体の数で止めるなら、止まったと報告する。報告は `docs/records/kernel-core-20261101/sharedmm-report.md`。

## 結論

- 大域の `shared_lock` と `shared_all` は消えた。共有 object の実装は `kernel/src/mm/` にあり、`sys/syscall.c` は 3210 行から 1287 行になった。
- 合成の道、共有の fault、text の fault は、割り込みを開けたまま object ごとの錠で走る。
- `vmm_demand_fault` は frame を空間の錠の外で 0 で埋める。
- 読み込みの窓の `vmm_demand_fault` の `spin_unlock` の標本は、前の 433・147（`serial-t11-nvme.log`）から今の 0・0（`serial-a.log`）になった。
- 0 埋めの仕事そのものは減っていない。今の読み込みの窓では `memset` が 480 標本である。
- 変わったのは、その仕事が空間の錠の外で、割り込みを開けて走ることである。
- `scripts/test.py` はフックが止めた。理由は媒体の数である。代わりに同じ試験のソースを窓付きの QEMU の Terminal で組んで走らせた（下の「試験」）。
- `threadtest` は `errors=1` で失敗する。同じ試験は main の媒体でも同じ行で失敗するので、今回の変更が原因ではない。

## 設計

ABI は変わらない。`docs/nocturne-native-abi.md` は変えていない。

### 部品

| ファイル | 責務 |
|---|---|
| `mm/shared.c` | scope、binding、leaf、DMA の hold、object、kernel の面、BAR、view、compose、revoke、withdraw、device の pin、scanout、claim の revoke、map、unmap、process の終わり、`shared_fault`。 |
| `mm/pager.c` | ページャー、source の待ち、fault の待ち。 |
| `mm/program.c` | program の像の共有と text の埋め方。 |
| `mm/space.c` | アプリの配置（`memory_reserve`、`memory_release`、`memory_protect`、空き区間）。 |
| `mm/shared.h`・`mm/space.h` | 公開の口。 |
| `mm/shared_parts.h` | 部品どうしの見出し。 |
| `sys/syscall.c` | 引数の検査と受け口（`sys_pci_bar`、`sys_dma_*`、`sys_shared_*`、`sys_pager_*`）。 |
| `sys/process.c` | `process_pages`、`process_memory_report`（`report_memory_exhaustion` の持ち主を移した）。 |

### 錠の順

先に取るものから後へ。`mm/shared.c` の頭と `arch/spin.h` の rank に同じ順を書いた。

```
owner->space.lock（無 rank。割り込みを閉じる）
  → owner->mappings_lock
  → p->lock（object ごと。二つ同時に持たない）
  → g->lock（ページャーごと）
  → device_pin_lock
  → SPIN_THREAD
  → 頁表の錠 → pmm → heap → vmalloc
```

- `mappings_lock`、`p->lock`、`g->lock`、`device_pin_lock` は割り込みを開けたまま持つ。
- これが成り立つ規則を `arch/spin.h` の頭に書いた。規則は、handler が取らない、rank が handler の取る錠より低い、handler の取る錠の持ち手がこれを待たない、の三つである。
- `thread_lock` を取る所は全部 thread の文脈である。`owner->space` の待ち行列は deadline 待ちが無く、handler が起こさない。
- compose と部分の object は錠を入れ子にしない。object の錠を二つ同時に持つ道は無い。
- scope の木は root の mutex `tree` で守る（木ごと）。revoke と claim の終わりは大域の mutex `revoke_mutex` を通る。
- mutex の順は `revoke_mutex` → `tree`、`claim->shared.lock`、`programs_mutex`。

### frame の持ち方

- RAM の `pages[]` は固定である。
- demand と paged の leaf の表（`shared_table`）は錠の外で作り、compare and exchange で公開する。
- `demand_make` が錠の外で frame を用意し、`demand_adopt` が公開する。競り負けたら返す。
- `resident` は atomic で数える。

### fault

1. `mappings_lock` で `span_at` を引く。
2. `p->lock` へ手渡しする。
3. `building`、`leaving`、`cancelled` を見る。
4. paged なら `g->lock` で entry を読む。結び付いていれば map し、無ければ `p->lock` を放して `pager_wait` で待つ。
5. revoke された device なら `gone_device_page` を錠の外で作り、compare and exchange で公開する。
6. demand で frame が無ければ、object と source の参照を取り、錠を放し、`program_fill`（text）か `demand_make` で用意し、最初からやり直す。
7. frame があれば `p->lock` の下で `vmm_map_page` を呼ぶ。

fault は祖先を歩かない。revoke が `cancelled` を各 object へ押す。

### revoke

- `shared_revoke` は scope の木と依存の辺を押して歩く。作業の山は `revoke_mutex` の下にある。
- `scope_visit` が `tree` の下で `revoked` と `owner->cancelled` を立て、children と dependents を積む。
- 錠を放してから `shared_withdraw` が写像を外す。TLB の撃ち落としは spinlock の外である。
- bindings は `bindings_ended` へ移し、`pager_source_revoked` を呼ぶ。
- claim の revoke は `shared_claim.ranges` から `withdraw_with_derived` で届く。段は range → composite → view の 3 段である。
- 全 object を歩く道は無い。

### map と unmap の順

- map：spares を用意する。`shared_dma_hold` と device の pin を取る。space → mappings → p で `space_take_locked`、span、一覧に載せる。各頁を `p->lock` の下で `m->cancelled` を見て map する。最後に space → p で `building=false` にする。
- unmap：`vmm_user_free` を呼ぶ。`p->lock` で `busy==0` を待ち `link_del` する。space → mappings → p で `span_remove` する。`hole_add`、`space_op_remove`、wake の後に、DMA の maps と pin を戻し、`object_unref` し、access の leaf を返す。

### demand の道

- `demand_fill` を消した。
- `vmm_demand_fault` は `walk` で leaf を見て、`DEMAND_AROUND`（16）個まで `pmm_alloc_zeroed` を錠の外で呼ぶ。
- 2 個目以降は `pmm_free_pages() >= 4096` のときだけ用意する。
- 1 個も用意できなければ錠を取らず 0 を返す。
- 錠の下で各 leaf を見直して載せ、余りを錠の外で `pmm_free` する。
- `vmm_user_alloc` の demand の leaf も、錠の外で用意した frame で載せる。

### 私が決めた点

持ち主の明示の承認の後に実装する約束があるが、この依頼は親からの指示で形が決まっており、私は持ち主に聞けない。細部の次の判断は私が決めた。

- `mappings_lock` を process ごとに置き、写像の一覧を空間の錠から分けた。
- 割り込みを開けたまま持つ錠の規則を `arch/spin.h` に書いた。
- `revoke_mutex` と `programs_mutex` を大域の mutex のまま残した。fault と合成の道は、前者を通らず、後者を記憶が足りないときだけ通る。

## 変えたファイル

新しいファイル：`kernel/src/mm/shared.c`（1403 行）、`shared.h`（137 行）、`shared_parts.h`（247 行）、`pager.c`（473 行）、`program.c`（280 行）、`space.c`（320 行）、`space.h`（17 行）。

変えたファイル：`kernel/src/sys/syscall.c`、`syscall.h`、`process.c`、`process.h`、`proc.c`、`thread.c`、`kernel/src/arch/spin.h`、`kernel/src/mm/vmm.c`、`vmm.h`、`kernel/src/dev/pciuser.c`、`pciuser.h`、`kernel/src/gui/wm.c`、`.claude/rules/map.md`、`.claude/rules/kernel.md`、`tests/threadtest.c`。

- `.claude/rules/map.md` に `mm/` の行を足した。
- `.claude/rules/kernel.md` は lock の順の行と `shared_view` の例を今のコードの事実に合わせた。規則を足してはいない。
- `tests/threadtest.c` は別のコミットである。8837d5bd から tcc でも clang でも組めず、`first`・`second` の二重の宣言を直した。

## 消したもの

`shared_lock`、`shared_all`、`pagers` の一覧、`SPIN_SHARED`、`shared_typed`、`demand_fill`、`sys_shared_cleanup`、`shared_demand_fault`、`report_memory_exhaustion`。

## 組みの結果

- `python -X utf8 scripts/build.py -j 8 kernel`：警告なし。
- `python -X utf8 scripts/build.py -j 8 qemu-image`：`IMG ../../../build/wt/agent-aaa78dd21d48e1d41/nocturne.img (1216 MiB)`、`C: 197 folders, 2747 documents, 938573741 bytes`。
- 組んだ後に変わったソースは `tests/threadtest.c` だけである。これは媒体に入らない。
- `git merge-tree` で main の 5cd02f38 との衝突は無い。

## QEMU で見たもの

条件：q35、4 CPU、4 GiB、WHPX、NVMe、VGA 2560×1440、窓付き、cmdline に `prof`。serial は `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\sharedmm\serial-a.log`。

画像はすべて `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\sharedmm\` の下にある。

| 見たこと | 画像 | 結果 |
|---|---|---|
| デスクトップ | `d1.png` | 正しく出る。 |
| Terminal を開く | `t1.png` | 開く。 |
| 窓を動かす | `t2.png` | 動いた。 |
| 大きさの変更 | `t3.png`、`t4.png` | 大きくなり、中身が描き直された。 |
| clang | `t4.png` | `clang t.c -o t` で組み、`.\t` が `hello clang` を出した。 |
| 窓を閉じる | `c0.png` | 閉じ、タスクバーからも消えた。 |
| 3D Cube | `c1.png` | 75 fps と表示。 |
| ブラウザーを開く | `b0.png` | about:home が出た。 |
| Wikipedia の Moon（最大化、F12 の Console を開く） | `w1.png` | 頁と画像が出た。 |
| スクロール | `w2.png`、`w3.png` | 本文が動いた。 |

- `serial-a.log` に `panic`、`[shared]`、`[memory]`、`[program]`、重さ E の行は無い。
- console の行：`[trace] #3 @216.449 JavaScript caught exception: at <anonymous> (https://en.wikipedia.org/w/load.php?...)`。これは頁の JS の行で、前の回との比べはしていない。

合成の行（`serial-a.log`）：

```
[  175.674218 1:17] wm: screen 0: 750 frames in 10000 ms, drawing avg 156 max 526 us, writing avg 548 max 4226 us, 0 longer than a refresh
[  185.675742 2:17] wm: screen 0: 741 frames in 10001 ms, drawing avg 155 max 559 us, writing avg 767 max 140819 us, 1 longer than a refresh
[  216.795076 2:17] wm: screen 0: 128 frames in 10050 ms, drawing avg 722 max 4425 us, writing avg 901 max 10148 us, 0 longer than a refresh
```

- 3D Cube の 750 frames の窓は、`compositor-report.md` の 750 frames の窓（drawing avg 177 max 335 us、writing avg 621 max 4189 us）と同じ桁である。別の回なので、速くなったとは言わない。
- 185 秒の窓の `writing max 140819 us` は、`compositor-report.md` にある `writing max 141868 us` と同じ形である。書込みの一回が長い件で、原因は今回調べていない。
- 読み込みの 216 秒の窓で一周期を超えたコマは 0 である。

## prof の標本

前は `serial-t11-nvme.log`（9f480e75 の kernel、Wikipedia の Moon）、今は `serial-a.log`（この枝の kernel、Wikipedia の Moon）。どちらも 4 CPU の合計で、窓は 10 秒である。

| 回 | 窓 | user | kernel | `vmm_demand_fault` の `spin_unlock` | `memset` | `serial_drain` | switches | TLB | throttles | late jobs |
|---|---|---|---|---|---|---|---|---|---|---|
| 前 | 78 秒 | 1108 | 744 | 433 | 上位に無し | 上位に無し | 9809 | 90 | 0 | 0 |
| 前 | 88 秒 | 4692 | 447 | 147 | 上位に無し | 上位に無し | 8904 | 89 | 0 | 0 |
| 今 | 216 秒 | 3525 | 924 | 0 | 480 | 12 | 13527 | 152 | 31 | 51 |
| 今 | 226 秒 | 6699 | 740 | 0 | 26 | 490 | 8065 | 39 | 27 | 46 |

読み方は次のとおりである。

- 前の 433・147 は、割り込みを閉じた区間の 0 埋めが、割り込みを開けた点へ寄った標本である。標本は割り込みで取るので、閉じている間は取れない。
- 今は 0 埋めが割り込みを開けて走るので、標本が `memset` そのものに出る。
- 0 埋めの仕事の量は減っていない。216 秒の `memset` 480 は前の 433 と同じ桁である。
- 減ったのは、空間の錠を持つ時間と、割り込みを閉じる時間である。
- `memset` の呼ぶ側は標本に出ない。`prof: caller` はアプリの番地だけを出す。480 のうち demand の分は分からない。
- 今の 216 秒の kernel の残りは `memcpy` 154、`e1000_poll_rx` 55、`e1000_send` 31、`mutex_lock` 31、`vbe_flip` 29 である。
- 今の 226 秒の `serial_drain` 490 は prof 自身の行の吐き出しである。これを除くと 226 秒の kernel は 250 である。

比べの限界は次のとおりである。

- 別の回で、ネットの速さと読み込みの山の窓が同じではない。
- 前の kernel は 9f480e75 で、この枝の土台 5043e3e6 より前である。
- `serial-t11-nvme.log` に `wm: screen` の行は無い。前の kernel に合成の締切の級は載っていない。
- 前の回の throttles 0 と late jobs 0 は、この比べに使えない。
- 比べられるのは `compositor/serial-after-1vga.log` の 340 秒の読み込みの窓で、kernel 1142、switches 14394、throttles 36、late jobs 107 である。今の 216 秒は throttles 31、late jobs 51 である。
- `shared_lock` の待ちの時間を直接測る計器は無い。錠が合成の道から消えたことはコードで確かめた。

静止の窓は kernel が 4〜6（前）に対し、今の 246 秒の窓は読み込みの後で kernel 43、user 718 である。今の回に同じ静止の窓の比べは取っていない。

## 試験

`scripts/test.py` はフックが止めた。

```
python -X utf8 scripts/test.py --accel whpx --quick --no-net --boot-image .../build/wt/agent-aaa78dd21d48e1d41/nocturne.img nativeabi sharedpages pager threads mem
止めた：媒体は 2 つまで。build に既に mae.img、nocturne.img があり、道具の写しが三つ目になる。
```

代わりに、媒体を増やさない道で試験のソースを走らせた。窓付きの QEMU（`snapshot=on`）の Terminal で、`fetch` で host の `tests/` から取り、`tcc` で組み、走らせた。serial は `serial-tests.log`。

| 試験 | 結果 | 画像 |
|---|---|---|
| `tests/pagertest.c` | `ページャー 102 検査 0 失敗`（期待の 3 つの fault を含む） | `x3c.png` |
| `tests/sharedpagetest.c` | `共有ページ 43 検査 0 失敗`（期待の fault を含む） | `x3c.png` |
| `tests/nativeabitest.c` | `339 checks, 0 failed` | `x3c.png` |
| `tests/nativechanneltest.c` | `333 checks, 0 failures` | `x6c.png` |
| `tests/wxtest.c` | `wxtest: 0 failed` | `x4c.png` |
| `tests/memreservetest.c` | `27 passed, 0 failed` | `x7c.png` |
| `tests/malloctrimtest.c` | `62 passed, 0 failed` | `x5c.png` |
| `tests/threadtest.c` | `FAIL errors=1` | `x9c.png` |

- `serial-tests.log` に `panic`、`[shared]`、`[program]`、重さ E の行は無い。
- `[memory]` の行は `memreservetest` が大きな予約をわざと断られた時の行である。
- `[segfault]` の行は `wxtest` などがわざと起こした fault である。
- この道は `test.py` の全部の代わりではない。群の残り（`systemwatchtest`、`stdiotest`、`entrywatchtest`、`memtest`、`cpuinfotest`）は走っていない。後の `store check` も走っていない。

### threadtest の失敗

失敗の行を出す写し（`serve\ti.c`）を、今の媒体と main の媒体（`build/nocturne.img`、`snapshot=on`、serial は `serial-mainbase.log`）で走らせた。

| 媒体 | 回 | 失敗 | 画像 |
|---|---|---|---|
| 今 | 1 | `bad line 365`（計器なしの回）、resources 0 | `x9c.png` |
| 今 | 2 | resources 45、計 46 | `x10c.png` |
| 今 | 3 | `bad line 365`、resources 1、計 2 | `x11c.png` |
| main | 1 | resources 184、計 185 | `m1c.png` |
| main | 2 | `bad line 365`、resources 0、計 1 | `m2c.png` |

二つの失敗はどちらも main の媒体でも出る。今回の変更が持ち込んだものではない。

1. `threadtest.c` の 365 行：`thread_stack_bounds(&unchanged, NULL)` の後で `unchanged` が変わる。`SYS_THREAD_STACK_BOUNDS` は `low` を写してから `high` を写すので、`high` が悪い番地だと `low` だけ書く。受け口のコードは土台と同じである。
2. `resource-detail check=path`：`n_path_complete("probe")` の結果が、folder を変える別の thread と競って期待と違う。回によって数が 0 から 184 まで動く。原因は調べていない。

どちらも範囲の外なので直していない。

## 残した項目と未確認

残した項目：

- `revoke_mutex`、`programs_mutex`、device の pin の一覧は大域である。fault と合成の道は `revoke_mutex` を通らない。`programs_mutex` は記憶が足りないときだけ通る。
- ページャーの faults の一覧はページャーごとの線形の一覧である。
- `vmm_map_page` の頁表の確保は空間の錠の下で行う（2 MiB に一度）。
- `shared_object` の大きな即時確保に中断点が無い。
- `memset` の 480 標本は減っていない。0 埋めの量を減らすなら、0 にした frame を前もって持つ別の設計になる。今回は入れていない。

未確認：

- `shared_scanout_open` と GPU の driver の道は QEMU の VGA では通らない。コードの形は保ち、組みは通した。動く確かめは実機が要る。
- claim の revoke（`tests/pcitest.c`）は走っていない。
- `tests/nvrm_*hosttest.c` は走っていない。
- 二枚の画面、周波数の違う画面、VRR、NUMA の多い機械は確かめていない。
- Hyper-V と実機は確かめていない。
- 1024 CPU の負荷での錠の待ちは測っていない。
- Terminal の日本語の字形は潰れて出る（`x3c.png` の `ページャー`）。今回の変更と関わるかは見ていない。
