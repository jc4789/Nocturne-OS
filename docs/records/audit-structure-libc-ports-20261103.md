# 構造の監査：libc、プログラムの土台、移植の接着、ビルド

版は `9be8a389`。読むだけの監査で、ビルド、QEMU、Hyper-V は動かしていない。

## 1. 依頼文

あなたは Nocturne OS（作業場所 `D:\Programes\llm vibe slop os`）の構造の監査を一人で担当します。読むだけの監査です。今日は 2026-11-03 です。

### このプロジェクトの持ち主の言葉（原文）

"what we need to do now, is to continue to audit, so that we know what the problems are structurally. ... there are to many layers that keep geting blured or done halfassed and its become an issue. things are allways added on top, nothing is ever refracted when needed or anything ... we just keep strut'ing and glueing ourselving into slop. everything seems likes its dont with a LOT of shortsightedness and no consideration as for why. ... this is a pervasive issue with how implementations are done, how code is written, and lack of consideration as to what purpose the code is serving."

### あなたの範囲：libc、プログラムの土台、移植の接着、ビルド

- `user/libc`（`user/libc/web` を除く。約 3 万 3 千行）、`user/include`、`user/` の直下のその他（約 5 千行）、`user/apps` の小さな道具（`ls.c`、`cat.c`、`sh.c`、`ps.c` など）
- `ports/` の全部（`ports/nvidia` 約 1 万 6 千行、`ports/mesa`。`ports/mesa/generated` は中身を読まず、置き方だけを見る。`wabt`、`go`、`rust`、`llvm`、`ffmpeg`、`typescript`、`lexbor`、`tcc`、`virgl`、`expat`、`freetype`）
- `Makefile`、`scripts/build.py`、`scripts/layout.py`、`ports/mesa/nvk.mk`、`user/image.map`

答える問い：
1. libc の層。ISO C、ネイティブ ABI の包み、GUI、媒体、網、HTTP、TLS、ブラウザーの保存が一つに入っている。各プログラムは全部を lazy な member として link する（Makefile の `LIBC_LINK`）。この形は何のためか。何が困るか。分けるなら責務・寿命・所有権でどう切れるか。共有ライブラリーは無いのか。同じコードが各プログラムに何度入っているか。
2. UNIX・POSIX・Linux の形が中に入っていないか（`AGENTS.md` が禁じている）。名前を変えただけの POSIX の口、`getenv`、ファイル記述子の形、`sh.c` と小さな道具の形。
3. shim と偽物。他の部品の見出しになりすます代役、何もしない lock、偽の関数、偽の環境で組むこと（`AGENTS.md` の定義）を、`ports/` の全部で挙げる。`ports/nvidia` の `nvkms_os.c`、`uvm_os.c`、`display.c`、`nvk_image.c` と `ports/mesa` の接着は全部読む。一つずつ、本当に動く実装か、何もしないか、嘘の値を返すかを分ける。
4. ドライバーの土台。アプリのドライバーの ABI（`SYS_PCI_*`、`SYS_DMA_*`）の上に `nvrm` がどう建っているか。層はいくつあり、それぞれ何のためか。二枚目のドライバーが来たら何が変わるか（`docs/records/audit-gpu-generic-20261102.md` と同じ所見は引くだけにする）。
5. 呼ばれないコード。どのプログラムからも呼ばれない libc の関数と、ヘッダーに公開されたまま実装や読み手の無いもの。一つの呼ぶ側のための包み、渡すだけの関数。
6. ビルドの形。`Makefile` の大きさと規則の形、生成物、`ports/mesa/generated` が git に入っている理由、移植の組み方。
7. 継ぎ足しの跡。`user/libc/sys.c`（62 コミット）、`user/include/nocturne.h`（75）、`Makefile`（82）などの履歴を `git log` で読む。

### 守ること

- コードを変えない。ビルド、QEMU、Hyper-V を動かさない。git の状態を変えない。書いてよいのは下の報告のファイル一つだけ。
- 先に `AGENTS.md` と `.claude/rules/map.md` を読む。
- 今あるコードに敬意を払わない。コメント、ファイルの頭の説明、docs、関数の名前は主張であって証拠ではない。証拠は動くコードで、誰が呼ぶか、誰がその値を読むかで確かめる。
- 構造は正しいのに読みにくいだけの所と、構造が間違っている所を分ける。正しい所は正しいと書く。
- 行ごとの不具合を並べない。書くのは構造の問題（層、持ち主、目的、重複、継ぎ足し、偽物）だけ。
- codegraph（MCP の `codegraph_explore`）と rg で探す。`SYS_*` の番号での呼出しと表からの呼出しは索引に出ない。
- Linux や他の OS のソースは読まない。移植の上流のソースは `D:\Programes\cloned repos` にある。
- 対象の外：ブラウザーの中（`user/libc/web`、`user/apps/browser*`）。ブラウザーだけのために libc にあるものは、libc の問題として書く。firmware の有無は話題にしない。特定のマシンに決め打ちした判断をしない。
- 全部を読めないなら、読んだ範囲と読まなかった範囲を正直に書く。推定は推定と書く。

### 報告

全文を `docs/records/audit-structure-libc-ports-20261103.md` に日本語で書く。Python で書くなら `newline='\n'` を付ける。節は次のとおり。

1. 依頼文（この文を全部そのまま）
2. 読んだ範囲と読まなかった範囲
3. 構造の問題。一つずつ：題、このコードの目的、今の持ち主とあるべき持ち主、証拠（`file:line`）、継ぎ足しの跡（コミットを見たなら hash）、消す範囲と書き直す範囲（ファイル、行数、数え方）、先に変える必要があるもの、確かさ（証明／可能性）
4. shim と偽物の一覧（場所、何のふりをしているか、実際に何をするか）
5. 残してよいもの（読んで構造が正しいと判断したものと、その理由）
6. 集計：部品ごとに、残す・書き直す・消すの行数の見積り
7. 依存の順序：どれを先に変えないと、どれを変えられないか

最後の返事は 15 行以内：ファイルのパス、重い問題の題、行数の集計。全文は返事に書かない。

## 2. 読んだ範囲と読まなかった範囲

### 全部を読んだ

- `AGENTS.md`、`.claude/rules/map.md`、`.claude/rules/nvidia.md`
- `Makefile`、`scripts/build.py`、`scripts/layout.py`、`user/image.map`、`user/cxx.ld`、`ports/mesa/nvk.mk`、`ports/nvidia/nvrm.mk`、`nvkms.mk`、`uvm.mk`
- `user/libc/sys.c`、`user/libc/net.c`、`user/libc/driver_image.c`、`user/libc/vulkan.c`
- `user/include/nocturne.h`、`errno.h`、`stdio.h`、`assert.h`
- `user/apps` の `ls`、`cat`、`ps`、`echo`、`rm`、`kill`、`dmesg`、`uname`、`true`
- `ports/nvidia/nvkms_os.c`、`uvm_os.c`、`display.c`、`nvk_image.c`、`os_stubs.c`、`nv-kthread-q-os.h`、`include/nvrm.h`、`include/nvrm_protocol.h`
- `ports/mesa/nocturne_gl.c`、`nocturne_os.c`、`drm-uapi/drm_fourcc.h`
- `ports/lexbor/memory.c`、`include/memory.h`、`ports/ffmpeg/include/assert.h`、`ports/tcc/config.h`、`runmain.c`、`include/limits.h`、`ports/expat/expat_config.h`、`ports/freetype/*`、`ports/wabt/include/*`
- `ports/nvidia/README.md` の構成、未対応、UVM の節
- `docs/records/audit-gpu-generic-20261102.md` の所見の題、`docs/records/gpu-compute-20261102.md`

### 一部を読んだ

- `ports/nvidia/uvm_os.h`：定義の並びの全部。本文は引いた行の周り。
- `ports/nvidia/nvrm.c`、`nvrm_serve.c`：起動、失敗の道、要求の受け、身元の照合。
- `ports/nvidia/uvm_shared.h`：頭の errno の表。
- 頭の説明と入口だけ：`os.c`、`os_ioctl.c`、`rm_api.c`、`os_caps.c`、`nvkms_kapi_rm.c`、`uvm_rm.c`、`uvm_serve.c`、`uvm_mm.c`、`spdm_crypto.c`、`libnvrm.c`（頭と UVM の所）、`third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_rm.c`
- `ports/wabt/interp-x64.cc`、`wasm_rt_nocturne.c`、`wasm_rt_symbols.c` の頭。
- `ports/mesa/generate.py`、`README.md`、`mesa.mk` の頭。
- `scripts/mksdk.sh` の頭 80 行、`link_unit.py`、`build_cxx.py` の頭。
- `user/libc` の `threads`、`malloc`、`services`、`parallel`、`gui`、`font`、`image`、`png`、`sound`、`wav`、`readline`、`stdio`、`keymap` の頭と一部。
- `user/apps/sh.c` の頭 200 行と grep の範囲、`term.c` の起動の所、`services.c` の起動の所。
- Go、Rust、LLVM、TypeScript の `README` と、差分の見出しの一覧。`posix_compat.h` の差分だけは本文を読んだ。

### 機械で見ただけで、読んでいない

- `ports/nvidia/os_pci.c`、`os_acpi.c`、`nvlink_os.c`：何もしない関数の抜き出しだけ。
- `user/libc` の `ui.c`、`media*.c`、`tls.c`、`http*.c`、`web*.c`、`json.c`、`deflate.c`：記号の表と行数と、呼ぶ者の検索だけ。中身の良し悪しは監査していない。
- `user/include` の残り：行数と読み手の検索だけ。

### 読まなかった

- `ports/mesa/generated` の中身（依頼のとおり）。
- `ports/go/nocturne.patch`、`ports/llvm/nocturne.patch`、`ports/rust/nocturne.patch` の本文の大部分。
- `ports/go/ui/ui.go`、`ports/go/apps`、`ports/rust/apps`、`ports/ffmpeg/prepare.py`、`vendor.py`、`ports/lexbor/vendor.py`、`ports/llvm/build.py`、`ports/virgl/build_runtime.py`、`scripts/mkimage.sh`
- `third_party` の上流の本文、`tests/` の本文。

### 測定の限界

- 重複の数は `build/symbols` の 80 本の記号の表から数えた。組みは 2026-11-02 11:18 で、`user/libc`、`user/apps`、`common`、`ports/mesa`、`ports/nvidia` のソースでこれより新しいのは `ports/nvidia/README.md` だけ。
- 記号は「名前と大きさが同じなら同じもの」と数えた。`static` の同名で大きさも同じ別物は混ざる。逆に、インライン化で名の変わった同じコードは数えない。
- 数えたのは記号の大きさで、RAM ではない。RAM は測っていない。
- 呼ぶ者は `.o` の未定義の記号と `rg` で探した。`SYS_*` の番号で呼ぶ Go の差分は別に検索した。表からの呼出しは名前が見えるときだけ拾える。

## 3. 構造の問題

### 結論

- 壊れているのはコードの中身より境界である。libc は共有コードの唯一の置き場になり、持ち主が無いまま全部が入った。
- 共有の仕組みが無いので、同じコードの記号の総和は一意の分の 2.55 倍ある。一つのプロセスに libc が二つ以上あり、heap も二つ以上ある。
- UNIX の形は `sh` と道具だけでなく、起動の ABI（環境、0・1・2、`terminals`）と網の口（`tcp_listen` の backlog まで）に入っている。
- 一番大きな偽物は UVM で、Linux カーネルの語彙を 5,436 行で再現し、製品に客が居ない。
- `nvrm` の中には OS の層が三つある。二枚目のドライバーが再利用できる土台は無い。
- libc を切る作業は 724 コミットの中に一度も無い。ABI を移す大きな作業はあったが、置き場所は変わらなかった。
- 行数で見ると、消すより移す方が多い。節 6 のとおり、書き直しは約 3 千行、消すものと判断の要るものが合わせて約 8 千 5 百行、置き場所を移すものが約 2 万 9 千行である。

### 問いと所見の対応

| 問い | 答え | 所見 |
|---|---|---|
| 1 libc の層 | 共有の置き場が他に無いのでこの形になった。切り方は S1 の表。共有ライブラリーは無い。同じ記号は最大で 80 本全部に入る。 | S1、S2、S3 |
| 2 UNIX の形 | 入っている。改名に当たるのは網の口。起動の ABI と道具は形を写している。 | S4、S5、S6 |
| 3 shim と偽物 | 節 4 の一覧。 | S6、S10、節 4 |
| 4 ドライバーの土台 | 描画の道で 9 段、画面と UVM が並ぶ。OS の層が三つ。二社目は土台を書き直す。 | S7、S8、S9 |
| 5 呼ばれないコード | 実装の無い宣言は調べた範囲で 0 件。読み手の無い実装は多い。 | S17 |
| 6 ビルド | 一枚の Makefile が全部の部品の旗を持つ。道具の有無で製品が変わる。古い出力が媒体に載る。 | S13、S14、S15 |
| 7 継ぎ足し | 問い 7 の小節。 | S11、S12、問い 7 |

### S1. libc は持ち主の無い束で、共有コードの唯一の置き場になっている

**目的。** 二つのプログラムが使うコードを置く場所が他に無い。`user/libc` に置けば Makefile を触らずに全プログラムから届く。使う分だけ結合されるので、小さなプログラムは小さいまま残る（`true` は記号の総和で 18.7KB）。

**今の持ち主。** 無い。`Makefile:126` の `find user/libc -name '*.c'` が、ディレクトリーの中の全部を同じ `LIBC_OBJ` にする。`Makefile:348-349` の `LIBC_LINK` は、そこへ BearSSL、QuickJS、Lexbor、Expat、FFmpeg、FreeType、WASM、C++ の実行時、Mesa、Zink、Vulkan の客を束ねる。

**あるべき持ち主。** 次の単位に切る。依存は表の上の方へだけ向く。

| 単位 | 責務 | 寿命 | 所有権 | 今の場所 | 行数 |
|---|---|---|---|---|---|
| 実行時と ISO C | C の標準、malloc、スレッド、stdio | プロセス | プロセスに一つ | `stdio`、`stdlib`、`string`、`math`、`time`、`threads`、`locale`、`wctype`、`malloc`、`setjmp`、`compiler_rt`、`cxxabi`、`crt0` | 8,128 |
| ネイティブの包み | 入口の溝の呼び方だけ | 呼ぶ間だけ。状態なし | ABI | `sys.c` の一部、`net.c` の頭 | 約 800 |
| サービスの口 | 管理者、利用者、設定、言語、音、網、GPU の像 | 接続ごと | 各サービスの ABI | `services`、`settings`、`language`、`audio`、`net` の残り、`vulkan`、`driver_image` | 約 1,300 |
| 端末の部品 | 行編集、文字の列 | 端末の接続 | 端末のプログラム | `readline`、`keymap` の `console_bytes` | 約 200 |
| 窓の部品 | 窓、UI、字体、画像 | 窓 | アプリ | `gui`、`ui`、`font`、`image`、`png`、`deflate` | 7,478 |
| 媒体 | decode、MSE、適応 | 再生 | 媒体の庫 | `media*`、`wav`、`sound` | 5,193 |
| 網の上の層 | TLS、HTTP、JSON | 接続と要求 | 網の庫 | `tls`、`http*`、`json`、`brotli_codec`、`third_party_brotli` | 4,568 |
| ブラウザー | 保存、取得、エンジン | 文書とプロファイル | ブラウザー | `web*.c`、`user/libc/web` | 6,979 と web の全部 |
| 第三者の庫 | 各自 | 各自 | 各自 | `LIBC_LINK` の残り | 上流 |

`parallel.c`（241 行）は名前を変えて「並列の庫」にするか、窓の部品の下に置く。今は段の名がブラウザーの語なので、段の表はブラウザーへ移す。

**何が困るか。**

- 層が逆に向く。
  - libc が子のプログラムを名指しで起こす：`user/libc/media_worker.c:78` の `browsermediaworker`、`media_mse_worker.c:72` の `browsermseworker`、`webnet.c:755` の `webfetch`、`webidb_async.c:89` の保存の子。
  - libc の公開の見出しの `parallel.h` が `PARALLEL_STYLE`、`PARALLEL_LAYOUT`、`PARALLEL_CSS_PARSE` を持つ。
  - `user/libc/font.c:8-9` の全体の錠の理由は「layout worker」で、同じファイルに `web_font_library` と `web_face` がある。
  - `nocturne.h:412-423` が TLS の番号を CSS、layout、WASM、Mesa に配る（S3）。
- 全プログラムの旗が全部の庫に合わせられる。
  - `Makefile:135-138` はどのプログラムにも BearSSL、画像、QuickJS、Lexbor、FreeType の見出しの道を足す。
  - `Makefile:280` はドライバーの像のために、web 以外の libc の全部を `-fPIC -fvisibility=hidden` で組む。
- 一つ変えると全部を結合し直す。`Makefile:356` の `$(LIBC_RSP)` は全メンバーに依存し、`Makefile:366` の全プログラムがそれに依存する。出力は 105 本ある。
- SDK が内部を公開する。
  - `scripts/mksdk.sh:7-9` のとおり、`libc.a` は web のエンジンと Mesa を含む。
  - `mksdk.sh` の `cp user/include/*.h` は `media_mse_worker_private.h` なども SDK へ入れる。
- 層を守る検査が無い。`tests/check_boundaries.py` が見るのはカーネルと `SYSCALL` 命令で、libc の層は見ない。

**継ぎ足しの跡。** 非 web のファイル数と web のファイル数は次のとおり伸びた。

| コミット | 日付 | 非 web | web |
|---|---|---|---|
| `1dd72cf3` | 10-05 | 10 | 0 |
| `325528cb` | 10-07 | 31 | 76 |
| `e1e01bb6` | 10-10 | 47 | 164 |
| `bbd4fb47` | 10-16 | 54 | 187 |
| `fd006ad8` | 10-19 | 55 | 190 |
| `9be8a389` | 11-03 | 72 | 241 |

分ける作業の跡は無い。git の改名の検出で `user/libc` に出るのは `http_gzip.h` から `http_coding.h` の一件だけ。`Makefile:315` のコメント「no archiver in the toolchain」は古い。`Makefile:5` に `AR := llvm-ar` があり、`scripts/build_cxx.py` は `libc++.a` を作る。

**範囲。** 中身はほとんど書き直さない。

- 移す：`user/libc` の ISO C 以外 約 2 万 6 千行、`user/include` の 2,838 行。
- 書き直す：`Makefile:126-138`、`:280`、`:315-366`（約 80 行）、`scripts/mksdk.sh` の頭（約 30 行）。
- 数え方は `wc -l` で、見積りは読んだ行の範囲から。

**先に要るもの。** S11（見出しを割る）、S13（Makefile を部品に割る）、S17（読み手の無いものを消す）。

**確かさ。** 束であること、層が逆に向くことは証明。切り方は提案で、依存を全部は確かめていない。

### S2. 共有の仕組みが無く、同じコードが何度も入り、一つのプロセスに libc が複数ある

**目的。** 静的結合で単純に保つ。像の記号を隔てて、Mesa の名がプログラムへ漏れないようにする。`tcc -run` を同じプロセスで走らせる。

**今の持ち主。**

- カーネルの `mm/program.c` は、同じプログラムのプロセスの間だけ読むだけの frame を共有する。違うプログラムの間では共有しない。
- `SYS_IMAGE_MAP` の像は `user/image.map` の `{ local: *; }` で何も出さず、自分の libc を一つ持つ。
- `ports/tcc/config.h:4` は「no dlopen: Nocturne has no shared libraries」と書く。共有ライブラリーは無い。

**あるべき持ち主。** プロセスに C の実行時は一つ。カーネルの「入口の像」（`kernel/src/sys/entry.c`）と同じく全プロセスへ写す、版つきの表を持つ OS の像に、実行時と包みを置く。ELF の動的リンカーの形を写さず、`n_image_map` の形を育てる。

**測定。** 80 本の記号の総和（名前と大きさで照合）：

| 項目 | 値 |
|---|---|
| 総和 | 215,712,672 バイト |
| 一意 | 84,467,354 バイト |
| 重複 | 131,245,318 バイト |
| 比 | 2.55 |

入っているプログラムの数：

| 記号 | 本数 | 記号 | 本数 |
|---|---|---|---|
| `malloc`、`memcpy` | 80 | `FT_Init_FreeType` | 22 |
| `n_result_text` | 50 | `stbtt_InitFont` | 22 |
| `printf` | 40 | `BrotliDecoderDecompress` | 22 |
| `win_event` | 22 | `font_draw` | 18 |
| `kBrotliDictionaryData` | 28 | `n_setting_get` | 13 |

重複の大きな記号：

| 記号 | 大きさ | 本数 | 何か |
|---|---|---|---|
| `.compoundliteral.25` | 4,920,750 | 2 | Mesa の `nir_opt_algebraic.c` の表。`browser`、`browserjsworker` |
| `lxb_unicode_table_map_110576_210042` | 397,864 | 10 | Lexbor の文字の表 |
| `kBrotliDictionaryData` | 122,784 | 28 | Brotli の辞書 |
| `film_grain_db` | 692,248 | 5 | FFmpeg |
| `face_cache` | 131,072 | 22 | `font.c` |
| `ft_adobe_glyph_list` | 55,997 | 22 | FreeType |

出力の大きさは `build/programs` 374MB（105 本）、`build/drivers` 14MB、`build/sdk` 280MB。

例を二つ挙げる。

- 窓の小さなアプリは記号の総和で約 0.91〜0.95MB ある。サービス `session` は 912KB で、上位は `face_cache`、`kBrotliDictionaryData`、`ft_adobe_glyph_list` である。Brotli の辞書は `ports/freetype/nocturne-ftoption.h` の `FT_CONFIG_OPTION_USE_BROTLI` から来る。Web の字体のための設定が、ブラウザーでないプログラムへ入っている。
- `shell`、`term`、`mandel`、`gpu_demo`、`agent` のファイルは約 2.8MB ある。`term` の記号の表に `unifont_rows` があり、`common/unifont.bin` は 2,162,688 バイトである。`snake` の記号の表に `unifont` は無い。

**複数の libc の帰結。**

- `user/libc/sys.c:26` の `libc_copy` と `:32` の `t->block.owner`、`:138-162` の `others` の鎖は、一つのスレッドの区画に複数の libc が乗ることを前提にする。
- `driver_image.c:45` のコメントが「There is one thread-local module per copy of the libc」と書き、`__tls_get_addr` は番号を捨てる。
- 像の heap で作った文字列はプログラムの `free` で解放できない。そのために `struct n_gpu_driver` に `log_free` がある（`ports/nvidia/nvk_image.c:23`、`ports/mesa/nocturne_gl.c:127-135`）。ドライバーの表に libc の境界の都合が出ている。
- `ports/tcc/runmain.c:1-4` は、`tcc -run` のプログラムが自分の libc を同じプロセスに持つと書く。

**継ぎ足しの跡。** 像と `vulkan.c`、`driver_image.c` は `b8f0448a` で入った。`tcc -run` は `debfcdb3`。

**範囲。**

- 書き直す：`sys.c:14-238`（225 行）、`driver_image.c` 80 行、`vulkan.c` 116 行、`user/user.ld` の TLS の所、カーネルの入口の像の周り（カーネルの監査の範囲）。
- 新しく書く：OS の像の表と結合の規則（約 500 行の見積り）。

**先に要るもの。** S1、S3。GPU の監査の F3、F4。

**確かさ。** 重複と複数の libc は証明。RAM の費用は未測定。あるべき形は提案で、性能は測っていない。

### S3. TLS が二通りあり、番号の表が OS の見出しに使う者を並べる

**目的。** 頭のコメントの主張は「`tcc -run` のプログラムは ELF の TLS を結合できない」である（`nocturne.h:412-414`）。

**今の持ち主。** `nocturne.h` が一か所で番号を配る。

- `N_TLS_WASM_RUNTIME`、`N_TLS_GL_CONTEXT`、`N_TLS_GL_DISPATCH`、`N_TLS_GL_CURRENT`、`N_TLS_CALL_ONCE`、`N_TLS_NET_CANCEL`（`nocturne.h:416-421`）
- コメントだけの番号：0、1、2 は「parallel, layout and CSS」、7 は C11 のスレッド。`parallel.c:6` は `POOL_TLS 0`、`threads.c:28` は `thread_local_get(7)`、`web/css.c:33` は `thread_local_get(2)`、`web/layout.c:39` は `thread_local_get(1)` と数字で書く。
- 上限は `N_TLS_SLOTS 16`（`nocturne.h:415`）で、AGENTS の「小さな固定の上限」に当たる。

**あるべき持ち主。** ISO C の `tss_*` か ELF の TLS の一つ。OS の見出しは使う者を知らない。

**証拠。** ELF の TLS は同時に動いている。`user/user.ld` の `.tdata` と `.tbss`、`ports/nvidia/os.c:50-53` の `thread_local`、`ports/nvidia/uvm_os.c:19-20` の `_Thread_local`、`ports/rust/x86_64-unknown-nocturne.json:39`、`driver_image.c` の `R_X86_64_DTPMOD64`。

**継ぎ足しの跡。** `N_TLS_SLOTS` は `e076f7b0` で入った。番号は部品が来るたびに足された。

**範囲。** 見出し 12 行。呼ぶ側は `threads.c`、`parallel.c`、`net.c`、`nocturne_gl.c`、`wasm_rt_nocturne.c`、`u_call_once.c`、`shared-glapi/core.c`、`web/css.c`、`web/layout.c`。

**先に要るもの。** TinyCC に TLS を入れるか、`tcc -run` の結合で何を許すかを決める。

**確かさ。** 二通りあることと番号の配り方は証明。`tcc -run` が今も理由になるかは未確認で、`third_party/tinycc/tccrun.c` を読んでいない。

### S4. 起動の ABI と道具が UNIX の形をしている

**目的。** 最初の `1dd72cf3`（題「GUI compositor/window manager, libc, shell, terminal and coreutils」）の coreutils が残っている。`bbd4fb47` で POSIX の syscall は消えたが、形は handle の番号へ移っただけである。

**今の持ち主。** 起動の ABI。端末の概念がカーネルの `struct n_launch` にある。

**あるべき持ち主。** 端末は一つのアプリの機能で、起動の ABI には載らない。子へ渡すものは型のある口（`n_cap_find` の形）にする。プログラムの場所は `programs` サービスが持つ。

**一覧。**

| 形 | 場所 | UNIX の何か | 判定 |
|---|---|---|---|
| `"NAME=value"` の列をカーネルが運ぶ | `common/abi.h:357-361`、`kernel/src/sys/syscall.c:858-861`（`=` まで確かめる）、`sys/proc.c:372-385` | `envp` | 形を写している。ISO C の `getenv` のためならカーネルへ入れる理由が無い |
| `getenv` で設定を読む | `ports/nvidia/nvrm.c:271`、`nvkms_os.c:158`、`os.c:1085`、`uvm_os.c:201` | 環境変数 | `abi.h:357-358` が「システムの部品はこれを読まず」と書くのに読む。`services.c:361-365` の起動は環境を渡さないので、値は必ず空である。F13 の根拠を足す |
| 位置で渡す handle。0 が入力、1 が出力、2 がログ | `abi.h:349-350`、`stdio.c:35-37`、`services.c:353`、`term.c:480`、Go の `README` | `dup2` と記述子 0・1・2 | 形を写している。ただし handle は `n_handle_kind` の型を持ち、全部がファイルではない |
| 3 番が管理者の channel | `common/services_abi.h:16` | 記述子の番号の決めごと | 同じ形。隣に型のある `n_cap_find` があり、口が二通りある |
| `terminals` と `n_terminal(h)` | `abi.h:354,365`、`sys.c:861-863` | `isatty` | 改名に近い |
| `interrupts` の channel | `abi.h:355,367`、`sh.c` の `take_interrupts`、`term.c:482` | `SIGINT` の代わり | ネイティブの形だが、端末の都合を起動の ABI に置く |
| `#!` の起動 | `sys.c:58-79`、`:441-460` | `execve` のスクリプト | libc の起動の中に UNIX の規則がある |
| `find_program` | `sys.c:632-643`、Go の `syscall.FindProgram`、`sh.c:508`、`term.c:478`、`gui.c:201` | `PATH` | `C:\Nocturne\System`、次に `C:\Programs` の各フォルダーを調べる固定の道 |
| `system` | `sys.c:674-677` | `sh -c` | 製品の呼ぶ側は無い。ISO C は命令の処理系が無いと答えることを許す |
| `sh` | `sh.c`（882 行）。pipe、`>`、`>>`、glob、`$?`、`&`、`$` のプロンプト、ANSI の列 | Bourne の縮小 | 形を写している |
| 道具 39 本、1,356 行 | `ls`、`cat`、`cp`、`mv`、`rm`、`mkdir`、`rmdir`、`echo`、`ps`、`kill`、`dmesg`、`uname`、`wc`、`grep`、`head`、`true`、`false`、`sleep`、`date`、`touch`、`clear`、`free`、`hexdump`、`host`、`ifconfig`、`lspci`、`ping`、`tree`、`uptime`、`reboot`、`poweroff`、`volume`、`neofetch`、`moonsay`、`fortune`、`fetch`、`play`、`beep`、`screenshot` | coreutils | 名前と旗を写している（`ls` のドットファイル、`rm -rf`、`echo -n`、`uname -a`、`ps` の PID・PPID・`zombie`） |
| 端末の列 | `keymap.c` の `n_console_receive`、`docs/nocturne-native-abi.md:284` | VT100 の列 | ABI の文書が `ESC [ A` を定める |
| `__syscall`、`sysinfo`、`proclist`、`yield`、`poweroff`、`reboot`、`readline` | `nocturne.h:389-394,430-437,448-449` | Linux と GNU の名 | `n_` の面の外に残った名 |

`ps.c` の `zombie` は `nocturne.h:125` の「no zombie」と食い違う。

**問題でないもの。**

- `errno.h:15-49` は ISO C++ の `<cerrno>` のために `E*` を `N_ERR_*` へ写す。形は許せる。
- ISO C の `getenv`、`stdio`、`system` の名そのものは標準である。問題は OS の部品がこれに頼ること。
- Go、Rust、LLVM の移植は `__nocturne__` と `n_*` の上に建ち、UNIX の族に入らない（`ports/llvm/README.md`、`ports/go/README.md`）。

**判断の線。** メモリーの規則は「最良と言える時だけ承認を得て借りる。似ているだけで消さない」である。だから道具の削除は判断が要る。消してよいと言える基準は「製品の窓から呼ばれず、試験も頼らない」である。`sh.c:872` の `run_wait("cat", ...)`、試験の `run_wait("screenshot", ...)`（`tests/guitest.c:69`）は頼る側の例である。

**継ぎ足しの跡。** `#!` は `2c5b39d1`（題は agent の Markdown）で入った。`3a4e8df8` の題は「設定の文書と環境の口を消す」だが、`__libc_environment` はこのコミットで入った。

**範囲。**

- 書き直す：`struct n_launch` と `struct n_start` の `environment`、`terminals`、`interrupts`、`sys.c` の 130 行、`stdio.c` の標準の handle の結び方。カーネルの `syscall.c:824-905`、`proc.c:341-397` はカーネルの監査の範囲。Go と Rust の差分の起動の所。
- 判断が要る：道具 1,356 行、`sh.c` 882 行。
- 移す：`syscallfuzz.c`（678 行）、`cppdemo` は `user/apps` から試験へ。

**先に要るもの。** 端末を製品に残すかの判断。`programs` サービスの口を `find_program` の呼ぶ側へ通すこと。

**確かさ。** 形があることは証明。「禁じられた改名」と言えるのは `n_terminal` と S5 で、残りは形の借用である。

### S5. 網の口は BSD ソケットの改名である

**目的。** プログラムへ TCP と UDP を渡す。`f591ba3f` の時から同じ形である。

**今の持ち主。** `common/abi.h:96-108`、`user/libc/net.c:48-77`、`nocturne.h:483-513`。

**あるべき持ち主。** 接続を型にしたネイティブの口。接続の失敗は結果で返し、待ちはカーネルの event で受ける。

**証拠。** AGENTS の「JUST Renameing POSIX... NEVER ALLOWED」に当たる。

| Nocturne | BSD |
|---|---|
| `udp_socket(port)` | `socket` と `bind` |
| `udp_sendto`、`udp_recvfrom` | `sendto`、`recvfrom` |
| `tcp_listen(ip, port, backlog)` | `listen(backlog)` |
| `tcp_accept(listener, deadline, remote)` | `accept` |
| `tcp_finish` | `shutdown` の書く向き |
| `tcp_address(s, local, remote)` | `getsockname` と `getpeername` |
| `struct n_sockaddr` | `sockaddr` |

足してあるのは期限と `SYS_TCP_CANCEL` で、ここはネイティブの工夫である。形の骨は変わっていない。

**継ぎ足しの跡。** `udp_recvfrom` は `f591ba3f`、`tcp_listen` は `a5be131c`（題に「Go の Listen」）。

**範囲。**

- C の呼ぶ側は `tls.c`、`network.c`、`timesync.c` と試験だけである。今なら安い。
- 書き直す：`net.c` の 40 行、`nocturne.h` の 30 行、`ports/go/nocturne.patch` の `SYS_TCP_*`（L5269-5495 付近）、`tls.c` の開く所。
- カーネルの `SYS_UDP_*`、`SYS_TCP_*` はカーネルの監査の範囲。

**先に要るもの。** S16（ABI の写し）。`SYS_*` の番号を変えるので、Go の差分が先に生成になると楽である。

**確かさ。** 改名の形は証明。カーネルの中の設計は読んでいない。

### S6. UVM は Linux カーネルの語彙の再現で、製品に客が居ない

**目的。** CUDA の統合メモリーを `nvrm` の中で動かす（`ports/nvidia/README.md:518`）。

**今の持ち主。** `ports/nvidia/uvm_*`。NVIDIA の UVM は RM と違って OS の層を持たず、Linux カーネルの API へ直接書いてある。だから移植は Linux の再現になる。

**あるべき持ち主。** 客が来たときに決める。候補は二つ。

- 統合メモリーを Nocturne の pager と RM の gpu-ops の上に自前で設計する。
- 上流の UVM を使うなら、AGENTS の shim の定義に当たるので、ユーザーが例外として決める。

**証拠：再現の範囲。** `ports/nvidia/uvm_os.h`（1,493 行）は頭で「Nothing here is POSIX」と主張する。中身は次のとおりである。

| 語彙 | 行 |
|---|---|
| `module_param`、`module_init` | L224-232 |
| `list_head`、`rb_node`、`radix_tree_root` | L369、L499、L535 |
| `task_struct`、`current`、`in_interrupt` | L611、L629、L639 |
| `NR_CPUS 256` | L663 |
| `mutex`、`rw_semaphore`、`kref`、`work_struct`、`delayed_work` | L758、L770、L860、L875、L880 |
| `page`、`device`、`resource`、`pci_dev` | L984、L1061、L1067、L1072 |
| `mm_struct`、`vm_fault`、`vm_operations_struct`、`vm_area_struct` | L1197、L1206、L1215、L1224 |
| `file_operations`、`address_space`、`cdev`、`inode`、`file` | L1321、L1334、L1346、L1352、L1364 |
| `poll_table_struct`、`seq_file` | L1393、L1417 |

`uvm_shared.h` は Linux の番号の errno を持つ。`uvm_serve.c:1-80` は `cdev_add` をリストで作り、接続を「task」と呼ぶ。`include/nvrm_protocol.h:25-40` の `file`、`minor`、`cmd`、`POLL` は `open`、`ioctl`、`mmap`、`poll` の形を channel へ写す。

**証拠：客が居ない。**

- `nvrm_uvm_open` を呼ぶのは `tests/nvrm_servetest.c` と `tests/nvrm_transport_hosttest.c` だけ（`rg` で確かめた）。
- `nvkmd_nocturne_rm.c` の `uvm` はコメントの中だけ。
- `ports/nvidia/README.md:601` と `docs/records/gpu-compute-20261102.md:51` が、客が無く実機で動かしていないと書く。
- それでも `nvrm.c:347` が毎回の起動で `nvrm_uvm_start` を呼び、`nvrm.c:65` の割り込みの道が `nvrm_uvm_interrupt` を呼ぶ。この費用は測っていない。
- 同じプロセスなので、運命を共にする。`uvm_os.c:123-133` の `panic` は `abort` するので、UVM の致命的な誤りは画面も RM も落とす。`uvm_os.c:969` はメモリーが無いときに `panic` する。落ちたときの道は F1 を引く。

**継ぎ足しの跡。** `8acc1589`（題は「RM の初期化後に表示を GPU の NVKMS の面へ移す」）で NVKMS、`display.c`、UVM が同時に入った。`uvm_os.c` は 3 回しか変わっていない。

**範囲（判断が要る）。**

- `ports/nvidia` の 5,436 行：`uvm_os.h`、`uvm_os.c`、`uvm_mm.c`、`uvm_serve.c`、`uvm_rm.c`、`uvm_rm.h`、`uvm_nvrm.h`、`uvm_shared.h`、`nv-kthread-q-os.h`、`uvm.mk`。
- `libnvrm.c:342-448` の 107 行、`nvrm_serve.c`、`nvrm.c`、`os_pci.c` の 9 行、`nvrm_protocol.h` の約 30 行。
- `third_party/nvidia-open/kernel-open/nvidia-uvm` の 223 ファイル 154,123 行と、その 40 ファイルの `NVRM_NOCTURNE`。
- 試験の該当の所。

**消しても残る道。** `docs/records/gpu-compute-20261102.md:11` のとおり、Vulkan の計算の道は UVM を通らない。メモリーの規則「GPU の道を消さない」に照らして、消すのは動いていない道だけである。これでも `docs/records/rust-cuda-20261102.md:101` の計画は UVM を当てにするので、判断はユーザーがする。

**先に要るもの。** ユーザーの判断。消すなら S7 の三重の土台の一つが自動で消える。

**確かさ。** 再現であることと製品に客が居ないことは証明。

### S7. `nvrm` の中に OS の層が三つあり、ドライバーの共通の土台が無い

**目的。** RM、NVKMS、UVM の三つの上流の部品を一つのプロセスで動かす。

**今の持ち主。** 三つの OS 層がそれぞれ持つ。

**あるべき持ち主。** プログラムのドライバーの共通の庫（ログ、作業キュー、タイマー、割り込みのスレッド、BAR と DMA の写し、接続の受け）。その上に NVIDIA の三つの口が薄く乗る。

**層（問い 4）。** 描画の道は 9 段ある。

| 段 | ファイル | 何のため | 行数 | 判定 |
|---|---|---|---|---|
| 0 | カーネルの `dev/pciuser.c`、`SYS_PCI_*`、`SYS_DMA_*` | PCI の権利、BAR、DMA、割り込み | カーネルの範囲 | 形は正しい |
| 1 | `sys.c` の `n_pci_*`、`n_dma_*` | 包み | 少し | 正しい |
| 2 | `os.c`、`os_pci.c`、`os_stubs.c`、`os_caps.c`、`os_acpi.c`、`nvlink_os.c`、`spdm_crypto.c` | NVIDIA の `os-interface.h` を満たす | 4,335 | 正しい継ぎ目。作業キューとタイマーは共通へ |
| 3 | `nv-kernel.o` | RM 本体 | 上流 | 部分リンクは正しい |
| 4 | `os_ioctl.c`、`rm_api.c` | Linux の `nv.c` の役 | 641 | 正しい |
| 5 | `nvrm_serve.c`、`nvrm_protocol.h`、`libnvrm.c`、`nvrm.h` | channel の上の RM の要求 | 1,012 | 形は残す。S8 |
| 6 | `nvkmd/nocturne` | NVK の裏の RM の客 | 3,503（`third_party/mesa` の中） | 頭だけ読んだ。未監査 |
| 7 | NVK、`nvk_image.c`、`vulkan.c`、`driver_image.c` | Vulkan と像 | 251 と上流 | F3、F4 |
| 8 | Zink、`nocturne_gl.c`、`js_webgl.c` | WebGL | 649 と上流 | 正しい |

並ぶ列が二つある。

- 画面：`nvkms_os.c`（1,044）、`nvkms_kapi_rm.c`（86）→ NVKMS → KAPI → `display.c`（1,903）→ `n_pci_give_screen` → カーネルの `fb`。
- UVM：`uvm_os.*`、`uvm_mm.c` → UVM → `uvm_serve.c`、`uvm_rm.c`（S6）。

**三重の証拠。**

| 土台 | RM | NVKMS | UVM |
|---|---|---|---|
| 作業スレッド | `os.c:726`（最大 8 本） | `nvkms_os.c:620`（`q_thread`） | `uvm_os.c:1282`（`work_main`）、`:1023`（`kthread_main`） |
| タイマー | `os.c:887` | `nvkms_os.c` の `q_*` | `uvm_os.c` |
| 設定 | `nvrm.c:271`、`os.c:1085` | `nvkms_os.c:158` | `uvm_os.c:201` |

`ports/nvidia/README.md:275` が自分で「汎用の `nv_kthread_q` 相当が OS 層にできれば、両方をそれに載せられる」と書く。設定は S4 のとおり必ず空である。

**二枚目のドライバー（引くだけのものは番号で書く）。**

- 同じ作り手の二枚目：変わらない。`services.c:65` の `SCOPE_FUNCTION` が機能ごとに `nvrm` を起こす。プロセスごとに装置一つという形は正しい。画面は F10（出力 0）で一つだけ。
- 別の作り手で変わる所：`services.c:59,65`（F2）、`vulkan.c`（F3）、`struct n_gpu_driver`（F4）、`NVRM_SCANOUT`（F5）、GPU の一覧（F6）、`nocturne.h:564` のコメント、`win_direct` の説明。
- 新しく足す所見：再利用できる土台がカーネルの ABI の外に無い。割り込みのスレッド（`nvrm.c:101`）、BAR の写し（`os_pci.c`）、ログ（`os.c:57-110`）、作業キュー、接続の受けと身元の照合（`nvrm_serve.c:158-166,297`）、部品の読込み（`os.c:1030`）は全部 NVIDIA の名のファイルにある。二社目はこれらを書き直す。推定で 3 千行から 4 千行である。
- ABI の名前も PCI に縛られる。画面の口が `n_pci_give_screen`、`n_pci_screen_*` である（`nocturne.h:171-237`）。PCI でない画面の装置は同じ口を使えない。カーネルの監査と合わせて見る。

**継ぎ足しの跡。** `fd006ad8` で `os.c`、`8acc1589` で `nvkms_os.c` と `uvm_os.c`。三つ目を足すときに、一つ目を分ける作業は無かった。

**範囲。**

- 書き直す：`os.c` の作業キュー、タイマー、ログ（200 行）、`nvkms_os.c` の `q_*`（250 行）。UVM を残すなら `uvm_os.c` の同じ所。
- 新しく書く：ドライバーの庫（約 800 行。大部分は移動）。

**先に要るもの。** S6 の判断、S1 の庫の置き方。F13、F15 は同じ時に直せる。

**確かさ。** 三重は証明。二社目の行数は推定。

### S8. channel の上の要求に、客の無い互換が残っている

**目的。** RM の `rm_ioctl` は引数の構造を受ける。channel に `NV_ESC_*` と引数を載せ、番地を arena の中だけ読み替える。ここの形は正しい（節 5）。

**今の持ち主。** `nvrm_protocol.h`、`nvrm_serve.c`、`libnvrm.c`。

**問題。** AGENTS は「互換は持たない」と決めている。次のものは互換のためにある。

- `include/nvrm_protocol.h:20`、`:92-97`（「Preserve Protocol1's size/offsets for statically linked apps」）。`union` で欄を流用する。
- `nvrm_serve.c:97`（「Old Protocol1 clients」）、`include/nvrm.h:30`（「old servers」）、`README.md:61-65`（「旧バイナリー」「旧クライアント」）。

客は `C:\Nocturne\Drivers\nvk` の中の `libnvrm` だけで、`nvrm` と一緒に組まれる。`docs/records/gpu-compute-20261102.md` のとおり、SDK から見出しは外された。古い客は居ない。

**継ぎ足しの跡。** `87dd1e2f` で `Protocol1` の語が入った。

**範囲。** 見出しの欄と説明で約 40 行、`nvrm_serve.c` の約 20 行を書き直す。

**先に要るもの。** なし。

**確かさ。** 証明。

### S9. `display.c` に画面の全部とサービスの方針が入っている

**目的。** KAPI の唯一の客として画面を持つ。これは正しい（節 5）。

**今の持ち主。** 一つのファイル（1,903 行）がモード、面、フリップ、カーソル、画像、panic、手放しを持つ。

**あるべき持ち主。** ドライバーは事実を返す。再試行の方針はサービス `display` が持つ。ファイルは KAPI の接続、モード、面とフリップ、直接表示に分ける。

**証拠。**

- `display.c:1378-1394` の `unshown_said`：サービス `display` の再試行の方針を知ったドライバーが、真実を言わない。`:149` に状態がある。`6e912418` で入った。
- 出力 0 の決め打ち：`:713`、`:1111`、`:1266`、`:1360`、`:1423`、`:1433`、`:1511`、`:1537`、`:1549`。F10 を引く。
- `:1745-1748`：起動の画面の GPU でなければ何もしない。
- 診断の残り：`head_state_log`（`:1201-1228`、CRC32）、`flip_seen_log`（`:808-827`）。
- KAPI の私的な構造を読む：`nvkms_kapi_rm.c` と `nvrm_nvkms_flip_*`（`:762`、`:869`、`:875`、`:1060`）。KAPI の表に足りないものを補うので、形は許せる。

**継ぎ足しの跡。** 11 コミット。`8acc1589`、`b13cfa79`（直接表示）、`c0ea9bde`（モード）、`5461059d`（多画面）、`d2ca6cee`（方針をサービスへ）。多画面の変更でも出力 0 は残った。

**範囲。** 書き直す 600 行（方針、出力、診断）。残り 1,303 行は分けて移す。

**先に要るもの。** F10、F5。サービス `display` の口はサービスの監査と合わせる。

**確かさ。** 方針の混ざりは証明。切り方は提案。

### S10. 偽の境界と偽の環境で組んでいる

**目的。** 上流の形を変えずに組む。

**今の持ち主。** `ports/nvidia/nvkms_os.c`、`nvrm.mk`、`nvkms.mk`、`uvm.mk`。

**あるべき持ち主。** 同じプログラムの中は直接呼ぶ。上流へ渡す定義は、上流の条件のうち本当に要るものを `third_party` で一度に直す（もう fork である。S14）。

**証拠。**

- `nvkms_os.c:727-745`、`:750-775`：同じファイルの中の `rm_ops` の表を通して呼ぶ。Linux のモジュールの境界の真似である。
- `nvkms_os.c:739`：同じ組みの版の文字列と比べる。必ず一致する。
- `nvkms_os.c:663-669`：ダミーのスタック。`rm_free_stack` は空である。
- `nvkms_os.c:295-303,667,698,720`：Linux の errno の整数 -22、-14、-12、-19。
- `nvrm.mk:60-66`、`nvkms.mk:63-67`：`-DNV_BSD`。理由は版の文字列を得るためだけと書く。
- `nvrm.mk:67`：`-DPORT_IS_KERNEL_BUILD=1`。`nvrm.mk:81`、`nvkms.mk:83`、`uvm.mk:53`：`-mno-red-zone`（プログラムなのにカーネルの習慣）。
- `nvrm.mk:5,11`：「until the link rule is added」「nothing from kernel-open」は古い。`README.md` の「llvm-nm は無い」もトップの `NM := llvm-nm` と食い違う。

**継ぎ足しの跡。** `8acc1589`。

**範囲。** `nvkms_os.c` の約 80 行、`.mk` の 6 行。

**先に要るもの。** なし。

**確かさ。** 証明。`-mno-red-zone` を外して RM が動くかは確かめていない。

### S11. `nocturne.h` は全部を一枚で公開する

**目的。** プログラムの入口を一つにする。

**今の持ち主。** 629 行の一枚。`abi.h`、`audio_abi.h`、`settings_abi.h`、`gfx.h` も読み込む。

**あるべき持ち主。** 口ごとの見出し（メモリー、文書、channel、アプリ、窓、音、網、PCI）。私的な見出しは部品の中へ。

**証拠。**

- 宣言は 222 個。`n_` が 146 個で、残りは `win_` 15、`tcp_` 11、`thread_` 9、`udp_` 5、`wav_` 5、`net_` 5 ほか。
- L111：宣言の無い孤立したコメント。L564：コメントが `nvrm` を名指しする。L412-423：TLS の番号。L519-534：`window_t` の中身を公開する。L599-628：音の効果と WAV。
- `user/include` の 5,327 行のうち、ブラウザーと媒体の私的なものが 2,838 行ある。
  - 名前に `private` が付く見出しが 8 枚ある：`media_mse_worker_private.h`、`media_worker_private.h`、`media_adaptive_private.h`、`media_mse_private.h`、`media_alloc_private.h`、`media_video_private.h`、`media_feed_private.h`、`media_rate_private.h`。
  - 他に `webnet_wire.h`、`js_worker_wire.h`、`browser_identity.h`、`web_inspect.h`、`web.h`、`webnet.h` ほか。
- `Makefile:561` は SDK の依存に `ports/nvidia/include` を今も持つ。`mksdk.sh` からは外れている。

**継ぎ足しの跡。** 90 コミット、109 行から 629 行。節の「問い 7」を見る。

**範囲。** `nocturne.h` 629 行を割る。全プログラムの include が変わる。正しい修理なので全体ビルドは理由にしない。

**先に要るもの。** なし。これが S1 の先。

**確かさ。** 証明。

### S12. `sys.c` は九つの責務を持つ

**目的。** ネイティブ ABI の包み。

**今の持ち主。** 1,020 行の一枚。

**あるべき持ち主。** 包みだけが残る。残りは次の場所へ移す。

| 行 | 責務 | 移す先 |
|---|---|---|
| L14-238 | TLS とスレッドの記録 | 実行時 |
| L245-625 | 包み | 残す |
| L58-79、L441-460 | `#!` の起動 | 消すか端末の部品（S4） |
| L632-672 | プログラムの検索 | `programs` の口 |
| L674-677 | `system` | 消すか無効を返す |
| L716-724 | 環境 | S4 |
| L726-835 | パスと file URL | パスの庫 |
| L842 | `n_app_data_folder` | サービスの口 |
| L877-1008 | 終了と起動 | 実行時 |
| L1011-1020 | メモリー | 残す |

**証拠。**

- L10-13：`int errno;` の「legacy dynamically compiled callers」。互換の主張である。
- L315-316：「SYS_ENTRY_INFO and SYS_FOLDER_LIST stay for programs built before its id」。`common/abi.h:323` と `kernel/src/sys/syscall.c:1330` に実体がある。AGENTS の「互換は持たない」に反する。
- L264：`n_result_text` が `strerror` を呼ぶ。ネイティブの層が ISO C の上に乗り、層が逆である。
- L964：`tcc -run` の特例。

**継ぎ足しの跡。** 75 コミット、161 行から 1,020 行。

**範囲。** 分割が 1,020 行、消すのが約 120 行。

**先に要るもの。** S11。

**確かさ。** 証明。

### S13. 一枚の Makefile が全部の部品の旗と例外を持つ

**目的。** 全部を一度に組む。

**今の持ち主。** `Makefile`（637 行）と、`include` する `.mk`。

**あるべき持ち主。** 部品ごとの `.mk` が自分の旗と出力を持つ。トップは順序と媒体だけを持つ。

**証拠（問い 6）。**

- 部品の旗が一か所に積もる：`:135-138`、`:198`、`:204-207`（媒体の 4 ファイルごとの上書き）、`:212-224`（`MEDIA_QUOTA_BYTES`、`-DMALLOC_PREFIX=nmedia_ff_`）。
- 道具の有無で製品が変わる：`:344`（C++ の実行時が `tools/llvm` の有無で変わる。F19）、`:422-487`（Go と TypeScript は入れ子の `ifneq`。コメントは「full system needs both」と書くのに任意）。
- 読むたびに走る `$(shell)`：`Makefile` に 16 か所（`:41`、`:126-127` ほか）。`mesa.mk`、`nvk.mk` に 2 か所ずつ。
- 古い出力が媒体へ載る：`scripts/layout.py` は `PROGRAMS.iterdir()` で何でも `C:\Nocturne\System` へ入れる。証拠は `build/c.list:76` の `C:\Nocturne\System\init`。`init` のソースは `16310d67` で消えた。`build/programs/init` は 2026-11-01 の組みである。
- 試験のプログラムが媒体へ載る：`:273-276` は `user/apps/*.c` の全部をプログラムにする。`syscallfuzz`、`gpu_demo`、`cppdemo` が入る。
- プログラムの表が三つある：`scripts/layout.py` の `APPS`、`user/apps/programs.c`、`find_program` のフォルダーの走査。
- 版が 3 か所：`Makefile:585-586`、`nvrm.mk` の `NVRM_VERSION`、`nvrm_os.h:26`。
- `:500-503`：`nvrm` の結合の規則だけがトップにある。
- `:621-631`：AGENTS が使わないと決めた `run` の目標。`scripts/qtest.py` も残る。
- `user/cxx.ld` は `INCLUDE user/user.ld` だけで、`:413-416`、`:514-517` の `patsubst` は意味が無い。
- `mksdk.sh` の `Source\Apps` は全プログラムのソースを SDK へ写す。SDK は 280MB ある。

**継ぎ足しの跡。** 88 コミット、88 行から 637 行。部品が来るたびに足された（`bad6bd0e`、`e82cdda7`、`6e074896`、`c31dddde`）。

**範囲。** 書き直す約 340 行、部品へ移す約 300 行。`layout.py` の 30 行（`APPS` を一つの表に、`PROGRAMS` を組みの一覧から）。

**先に要るもの。** なし。変えたら空の `build/` から組めることを確かめる（AGENTS）。

**確かさ。** 証明。

### S14. 移植の方式が四つあり、`third_party` は実は fork である

**目的。** 上流を取り込む。

**今の持ち主。** 部品ごとに違う。

**あるべき持ち主。** 一つの方式。上流の版、手入れの一覧、更新の手順が同じ形で読める。

**証拠。**

| 方式 | 部品 | 手入れの記録 |
|---|---|---|
| `third_party` を直接直す | mesa（102 ファイルに `nocturne`）、nvidia-open（44）、wabt（12）、ffmpeg（10）、tinycc（8）、lexbor（6）、quickjs（4）、bearssl（2）、expat（2）、brotli（2）、uacpi（1） | `ports/mesa/README.md` の表、`ports/nvidia/README.md`、`README.nocturne.md`（7 部品）、`third_party/lexbor/NOCTURNE_PATCHES.json` |
| `.patch` を当てる | go（87 ファイル）、llvm（103）、rust（38） | `README`。木は `tools/` の中で git の外 |
| overlay | typescript | `README`。`ports/typescript/nocturne.patch` は誰も読まない（`rg` で確かめた） |
| 構成と代役の見出し | ffmpeg、lexbor、tcc、expat、freetype | `ports/<名>` |

`map.md` の「取り込んだ上流のソース」という記述は、mesa については事実と違う。手入れの形も四通りである。

- 条件で隔てる（`#if !defined(NVRM_NOCTURNE)`、`__nocturne__`）。
- 同じ名の関数で差し替える（`ports/mesa/nocturne_os.c`）。
- 上流の本文を書き換える（softpipe のスレッド化。`ports/mesa/README.md` の表）。
- 上流の見出しを消して代役を置く（`ports/mesa/drm-uapi/drm_fourcc.h`）。

上流の版を上げる手順が機械になっているのは `.patch` の三つだけである。

**継ぎ足しの跡。** `git log` で `third_party/mesa` は 22、`third_party/nvidia-open` は 6 コミット。多くが上の部品の機能のコミットに混ざる。

**範囲。** 書き直すコードは少ない。手入れを `.patch` か一つの表に揃える作業で、見積りは出せない。

**先に要るもの。** なし。

**確かさ。** 数は証明。一つの方式に揃える費用は推定できていない。

### S15. 生成物の置き方が三通りある

**目的。** 空の組みで動かす。

**証拠（問い 6）。**

| 置き方 | 例 | 再生成 |
|---|---|---|
| `build/` へ出す | `build/mesa/gen`（`ports/mesa/generate.py`） | make の規則 |
| ソースの木へ書いて git に入れる | `user/libc/web/*.inc`、`user/libc/wctype_data.h`、`third_party/lexbor/source/lexbor/unicode/idna_validity_res.h`（`Makefile:287-308`） | make の規則。third_party の中へ書く |
| 手で作って git に入れる | `ports/mesa/generated` の 31,173 行 | `ports/mesa/README.md:22-26` の手のコマンド |

`ports/mesa/generated` が git にある理由：

- bison と flex の出力と `mesa_clc` の出力である。
- 道具は `tools/mesa-tools` にあり、`tools/` は git の外である。空の組みでも動くように、出力を入れた。
- make の規則は無い（`mesa.mk:8,66`）。`.yy` を変えても組みは古い解析器を黙って使う。
- 履歴は `c5d1c410`、`bbd4fb47`、`bad6bd0e`。

理由の所は `README` の記述と道具の置き場からの推定である。

`Makefile:286` のコメントは「in-OS linking without Python」を理由に挙げる。mesa の表は別の理由である。理由が二つあり、規則が三つある。

**範囲。** make の規則を足す約 20 行。`third_party` へ書く 1 行を `build/` へ移す。

**先に要るもの。** S13。

**確かさ。** 置き方は証明。

### S16. ABI の事実が手で写されている

**目的。** Go はカーネルの溝を直接呼ぶ。

**証拠。** `ports/go/README.md` は「番号は `common/abi.h` の値を差分の中に書く」と書く。`ports/go/nocturne.patch` の L3671 付近と L5269 付近に番号の表が二つある。Rust の `abi.rs` は `struct n_launch` と同じ形を持つ。生成器も検査も無い。`.note.nocturne` の世代（`common/abi.h:245`）は、番号の一つを足す変更では動かない。

**継ぎ足しの跡。** `d2171979`、`0b8d6f63`：ABI を変えるたびに差分を手で直している。

**あるべき持ち主。** `common/abi.h` から Go と Rust の表を生成する。

**範囲。** 生成器が約 100 行、差分の 60 行を生成物へ。

**先に要るもの。** なし。S5 と S4 の前にすると楽である。

**確かさ。** 手の写しは証明。Rust の形は差分の見出しと `README` から見た。

### S17. 同じ役の部品が二つあり、読み手の無いコードが残っている

**目的と今の持ち主。** 部品が来るたびに足され、古い方を消さなかった。

**二重の部品。**

| 役 | 二つ | 証拠 | 跡 |
|---|---|---|---|
| 字体の engine | stb_truetype と FreeType | `font.c:70`（`stbtt_InitFont`）、`font.c:128`（`FT_New_Memory_Face`）。22 本に両方入る | FreeType は `fe16789a`（題は DDG のリンクの修理） |
| 全体の字体の錠 | マクロで名を変えて包む | `font.c:10-26`、`:1432-1458`。`font_x` の全部が一つの錠を取る | `a8b239a3`（題は SVG の寸法） |
| 行編集 | `readline.c` と `sh.c:read_line` | `sh` は `readline` を呼ばない | `212c6323` |
| 3D のソフト描画 | `gpu.c` の `raster` と Mesa | `gpu.c:37`。F16 を引く | `325528cb` |
| プログラムの表 | 三つ | S13 | |

字体の全体の錠は並列の layout の待ちになりうる。測っていない。

**読み手の無いコード（問い 5）。** 25 枚の見出しの宣言を調べた。実装の無い宣言は 0 件である。正規表現で拾えない複数行の宣言とマクロは限界である。

| 分類 | 記号 |
|---|---|
| 定義のファイルの外に読み手が無い | `n_cap_kind`、`n_net_routes`、`n_settings_describe`、`tcp_accept`、`tcp_address`、`tcp_finish`、`tcp_listen`（Go は `SYS_*` を直接呼ぶ）、`win_pointer_shape`、`n_shell_notify`、`parallel_for_service`、`n_language_changed`、`n_locale_date`、`n_locale_month`、`n_locale_now`、`n_locale_number`、`n_locale_weekday`、`ui_blob`、`ui_strn`、`ui_window_close`、`jb_vprintf`、`json_str`、`webidb_async_pending`、`webidb_async_ready` |
| 試験だけが読む | `n_clock_boot`、`n_entry_slot`、`tcp_send`、`win_move`、`gfx_render3d`、`gfx_render3d_list`、`gpu_render_batch`、`gpu_render_list`、`parallel_call`、`parallel_enable`、`parallel_for`、`net_resolve_at`、`n_gpu_driver_load`、`font_advance`、`font_has`、`font_open`、`image_decode`、`http_pool_clear`、`http_request_h2_prior_knowledge_ex`、`http_request_limited`、`http_request_stream_timed`、`ns_close`、`ns_get_connection_info`、`ns_open_http_timed`、`ns_open_timed`、`ns_session_resumed`、`webnet_busy`、`webnet_cookie_get`、`webnet_cookie_set`、`webnet_inflight`、`webcookie_session_end`、`webprefs_site_rule_url` |
| libc の外の `.o` が呼ばない global | 327 個。ISO C の面（`math` 31、`stdio` 33、`stdlib` 17、`string` 23、`wctype` 13、`locale` 12、`threads` 6、`time` 7）は問題でない |
| 第三者の未使用 | `third_party_img` 28、`third_party_ttf` 34 の記号 |
| 死んだ関数 | `nvrm_nvkms_suspend`、`nvrm_nvkms_resume`（`nvkms_os.c:1034-1044`。呼ぶ者が無い） |
| 死んだ枝 | `nocturne_gl.c:631-649`（`zink.mk` が `-DGALLIUM_ZINK` を常に付ける） |
| 読まれない古いファイル | `ports/typescript/nocturne.patch`、`user/cxx.ld` |

名前の検索はコメントと文字列にも当たるので、数は少なめに出ている。「定義のファイルの外に読み手が無い」は確かな方である。

**一つの呼ぶ側のための包み。**

- `webidb`（18 個中 18）、`http2`（9 個中 9）、`json`（17 個中 15）、`webnet`（32 個中 28）、`media_worker`（19 個中 18）、`media_mse`（28 個中 24）、`media_mse_worker`（32 個中 29）は、呼ぶ側がほぼ一つである。公開の面でなく、持ち主の部品の中へ入れるものである（S1）。
- `sys.c` の一つの外の呼ぶ側だけのもの 65 個は、ABI の面なので問題でない。

**渡すだけの関数。** 本体が一文で別の関数を呼ぶだけのものは `user/libc` に 174、`ports/nvidia` に 131 ある。ほとんどは ISO C の別名（`getc`、`iswalpha`）か RM の名を満たす口（`nvkms_memcpy`、`nvlink_malloc`）で、形は正しい。残すべきでないものは `http_request_*` が 4 本とも `request_with_limit` へ渡す形で、読み手の無い入口が混ざる。

**範囲。** 消す 538 行（節 6）。字体の engine を一つにする約 500 行は書き直し。

**先に要るもの。** なし。一番先にできる。

**確かさ。** 読み手の無さは `.o` と `rg` の証明。字体の engine を一つにできるかは可能性で、FreeType に寄せたときの費用は測っていない。

### 問い 7：履歴から読めること

`git log` で数えた。依頼文の数は古い。

| ファイル | コミット（測定） | 依頼文 | 最初の行数 | 今の行数 |
|---|---|---|---|---|
| `user/libc/sys.c` | 75 | 62 | 161 | 1,020 |
| `user/include/nocturne.h` | 90 | 75 | 109 | 629 |
| `Makefile` | 88 | 82 | 88 | 637 |

行数の伸び：

| コミット | 日付 | `sys.c` | `nocturne.h` | `Makefile` |
|---|---|---|---|---|
| `1dd72cf3` | 10-05 | 161 | 109 | 88 |
| `325528cb` | 10-07 | 355 | 176 | 239 |
| `e1e01bb6` | 10-10 | 530 | 193 | 294 |
| `bbd4fb47` | 10-16 | 682 | 294 | 391 |
| `fd006ad8` | 10-19 | 700 | 312 | 489 |
| `e82cdda7` | 10-23 | 823 | 386 | 526 |
| `8acc1589` | 10-24 | 854 | 437 | 526 |
| `16310d67` | 11-01 | 894 | 544 | 600 |
| `d671419e` | 11-01 | 1,026 | 570 | 599 |
| `9be8a389` | 11-03 | 1,020 | 629 | 637 |

読めること：

1. 持ち主の言葉の「nothing is ever refactored」は半分正しい。
   - 大きな移動はあった。ネイティブ ABI の段（`04637114` から `bbd4fb47`）、`init` の削除（`16310d67`）、身元と能力（`d671419e`）、設定、音、画面、網のサービス化（`bb5286ac`、`d2ca6cee`、`6d1fa363`）。
   - その全部で、三つのファイルは伸びた。移動は「何を呼ぶか」を変え、「どこに置くか」を変えなかった。
2. libc を切る作業は一度も無い。git の改名の検出で `user/libc` は `http_gzip.h` の一件、消えた上の段の `.c` は `media_stdio.c` の一件である。
3. 設計の決定が、不具合の修理の題で入っている。

   | コミットの題 | 実際に入ったもの |
   |---|---|
   | `fe16789a` DDG のリンクの修理 | `font.c` の FreeType |
   | `a8b239a3` SVG の寸法の修正 | `parallel.c`、字体の全体の錠 |
   | `28d00eed` MSE の音声先行の修理 | `webidb.c` |
   | `c59d0dec` JavaScript Support | `webnet.c`、`webcookie.c` |
   | `2c5b39d1` agent の Markdown | `#!` の起動 |
   | `8acc1589` 表示を NVKMS の面へ移す | NVKMS、`display.c`、UVM（5,000 行超） |
   | `b8f0448a` アプリの境界の土台 | `vulkan.c`、`driver_image.c` |

4. `sys.c` と `nocturne.h` の履歴には全部の部品の題が並ぶ。音、画面、電源、設定、身元、時間、NVIDIA、LLVM、WASM、MSE、TCC、FAT32 である。一つのファイルを全部の仕事が通ることが、継ぎ足しの形の証拠である。
5. 並行の枝の統合（`担当1` から `担当4` の merge）が、`sys.c` と `nocturne.h` で衝突の場所になっている。一枚のファイルが衝突の的になる。`551083c0`、`541afabe`、`1aedbd88` など。

## 4. shim と偽物の一覧

分類は次のとおり。

- **動く**：本当に働く。
- **何もしない**：空で成功か、値を捨てる。
- **嘘**：事実と違う値を返す。
- **正直な無し**：未対応と言う。偽物ではない。
- **代役**：他の部品の見出しになりすます。
- **偽の環境**：違う環境の定義で組む。

### 代役の見出し

| 場所 | 何のふりか | 実際 | 分類 |
|---|---|---|---|
| `ports/nvidia/uvm_os.h`（1,493） | Linux カーネルの `linux/*.h` の全部 | S6 の語彙を定義する。`uvm_linux.h` がこれを選ぶ | 代役 |
| `ports/nvidia/uvm_shared.h`（124） | Linux の errno の番号 | enum | 代役 |
| `ports/nvidia/nv-kthread-q-os.h`（28） | kthread のキューの OS 部分 | C11 スレッドの構造 | 代役。動く |
| `ports/mesa/drm-uapi/drm_fourcc.h`（19） | Mesa の `include/drm-uapi/drm_fourcc.h` | Khronos の登録の値を数個だけ。頭で「stand-in」と認める | 代役。値は正しい |
| `ports/lexbor/include/memory.h`（5） | 歴史の `<memory.h>` | `string.h` を読むだけ。SDK にも入る | 代役。`third_party/lexbor` を直せば要らない |
| `ports/ffmpeg/include/assert.h`（5） | `<assert.h>` | `include_next` して `static_assert` を定義する。`user/include/assert.h` が同じものを持つ | 代役。読む限り要らない（組んでは確かめていない） |
| `ports/tcc/include/limits.h`（23）、`stdint.h`（62） | 標準の見出し | 本物。ISO C の面なのに `ports/tcc` にある。`SSIZE_MAX`、`PATH_MAX` は POSIX の名で、`user`、`ports/nvidia`、`ports/mesa` に読み手が無い | 置き場の誤り |
| `user/libc/third_party_ttf.c` の `FT_New_Memory`、`FT_Stream_Open` | FreeType の `ftsystem.c` | `malloc` の上の実装と、必ず失敗する open | 動く。正直な無し |

### `ports/nvidia/nvkms_os.c`（全部読んだ）

| 箇所 | 分類 | 内容 |
|---|---|---|
| オプションの解析 L50-172 | 動く | ただし読む環境は必ず空（S4） |
| ロック、作業スレッド、タイマー L357-652 | 動く | 三重の一つ（S7） |
| `ref_ptr`、KAPI の入口、`config_file` | 動く | |
| `nvkms_kernel_supports_syncpts`、`nvkms_syncpt_op`、`nvkms_get_per_open_data`、`nvkms_fd_is_nvidia_chardev` L219-248 | 正直な無し | |
| `nvkms_register_backlight` | 正直な無し | NULL |
| `nvkms_extcon_report_hdmi`、`_audio`、`nvkms_unregister_backlight` | 何もしない | HDMI の接続の知らせを捨てる |
| `rm_ops` の表と呼出し L727-745、L750-775 | 偽の境界 | 同じファイルの関数を表で呼ぶ |
| 版の照合 L739、L968-973 | 偽の検査 | 自分と比べる |
| ダミーのスタック L663-669、`rm_free_stack` | 何もしない | |
| Linux の errno の整数 L295-303、L667、L698、L720 | 偽の環境 | |
| `nvrm_nvkms_suspend`、`_resume` L1034-1044 | 死んだ | 呼ぶ者が無い。suspend の道が無い |

### `ports/nvidia/uvm_os.c`（全部読んだ。`uvm_serve.c`、`uvm_mm.c` は頭だけ）

| 箇所 | 分類 | 内容 |
|---|---|---|
| `node_possible_map` ほか L1096-1098、`cpumask_of_node` L1087-1094、`nv_kthread_q_init_on_node` L1642 | 嘘 | 常に node 0。カーネルには NUMA がある |
| `NR_CPUS 256`（`uvm_os.h:663`）、`uvm_os_cpu_count` L1074 | 嘘 | 固定の上限で切り詰める。AGENTS は 1024 の論理 CPU を求める |
| radix tree L822-902 | 動く、名が嘘 | 中身は rb tree |
| `kmem_cache` L1505-1547 | 動く、名が嘘 | キャッシュをしない |
| `vmalloc` L1416-1503 | 動く | `malloc` と木 |
| `sort` L1574-1579 | 動く | `swap_fn` を捨てて `qsort` を呼ぶ |
| `get_random_bytes` L1581-1597 | 嘘の余地 | `n_random` の失敗のとき、時計から作った列を返す。呼ぶ側は気づけない |
| `on_uvm_test_fail`、`on_uvm_assert` L242-250 | 何もしない | デバッガーの止まり場所で、形は上流どおり |
| `printk`、`dump_stack`、`panic` L31-133 | 動く | ログへ出す。`panic` はプロセスを落とす |
| ビットマップ、rb tree、rw semaphore、待ち行列、`kmalloc` L355-816、L1101-1215、L1348-1415 | 動く | C11 と `wait_on_address` の上の再現 |
| kthread、`task_struct`、作業スレッド、`nv_kthread_q` L934-1059、L1217-1346、L1617-1722 | 動く | 三重の一つ（S7） |
| `errno_to_nv_status`、`nv_status_to_errno` L284-342 | 動く | 再現の errno を戻す |
| `UVM_OPTIONS` L199-234 | 何もしない | 読む環境は必ず空 |
| `uvm_serve.c` の `unregister_chrdev_region` | 何もしない | |
| `uvm_serve.c` の `cdev_add`、接続の task | 動く | デバイスファイルの再現 |
| `uvm_mm.c` の `mm_struct`、`vm_insert_page` ほか | 動く | pager の上の再現。読んだのは頭だけ。実機で動かしていない |

### `ports/nvidia/display.c`（全部読んだ）

| 箇所 | 分類 | 内容 |
|---|---|---|
| モード、面、フリップ、カーソル、画像、panic、`hand_back` | 動く | 本物の KAPI の客 |
| `unshown_said` L1378-1394 | 嘘 | サービスの再試行のために事実を言わない |
| 出力 0 の決め打ち L713 ほか | 嘘に近い | F10 |
| L1745-1748 | 何もしない | 起動の画面の GPU でなければ何もしない |
| `head_state_log`、`flip_seen_log` | 診断の残り | 製品の働きに要らない |

### `ports/nvidia/nvk_image.c`（全部読んだ）

全部が動く。何もしない関数は無い。形の問題は二つで、番号で引く。

- `.log_hold`、`.log_release`、`.log_free` は Mesa の語を GPU の口へ出す（F4、F20）。
- `.scanout_*` は `nvrm` の状態を全部 `-N_ERR_IO` へ畳む（F5）。

### `ports/nvidia/os_stubs.c`（全部読んだ、115 関数）

形は正しい。RM の OS の口を満たす。空 29、成功か真 10、未対応か偽 17、`NOT_SUPPORTED` 55 ほか。問題になるのは次のものだけである。

| 関数 | 分類 | 内容 |
|---|---|---|
| `os_is_efi_enabled` L191 | 決めつけ | TRUE |
| `nv_is_chassis_notebook` L196 | 嘘の余地 | FALSE。ノートでも FALSE |
| `os_is_init_ns` L215 | 決めつけ | TRUE |
| `nv_indicate_idle`、`nv_indicate_not_idle` L220-221 | 何もしない | 成功を返す |
| `nv_get_updated_emu_seg` L44 | 何もしない | 引数を埋めない |
| `os_get_page_refcount`、`os_count_tail_pages`、`os_get_reclaimable_memory_usage`、`os_get_grid_csp_support`、`os_imex_channel_count` | 決まった値 | 0 |
| `nv_i2c_*`、`os_get_smbios_header`、`os_get_acpi_rsdp_from_uefi` | 正直な無し | |

### `ports/nvidia` の残り（機械の抜き出しで、本文は読んでいない）

| 場所 | 分類 | 内容 |
|---|---|---|
| `os_pci.c` の `os_io_read_*` は `0xFF`、`os_io_write_*` は空 | 決まった値 | I/O port を持たない |
| `os_pci.c` の `os_flush_user_cache`、`nv_get_dev_minor` | 何もしない | 成功か 0 |
| `os.c` の `os_dbg_init`、`os_dbg_set_level` | 何もしない | |
| `os_acpi.c` の `nv_acpi_is_battery_present` | 嘘の余地 | FALSE。電池のある機械でも FALSE |
| `nvlink_os.c` の `nvswitch_os_sync_dma_region_*` | 何もしない | 成功。装置が無いので届かない |
| `nvlink_os.c` のその他 | 正直な無し | |
| `nvrm_serve.c:104` の `client_is_admin` | 決まった値 | 引数を捨てて false。普通の客は管理者でない |
| `nvrm.c:324-329` | 嘘 | 起動に失敗しても true で、サービスは ready になる。F15 |
| `nvrm.c:45` の `IRQ_WAKE_MS 50` | 症状を隠す可能性 | 届かない割り込みに備えて 50ms ごとに RM の handler を回す。届かない原因は別に調べていない |
| `rm_api.c` の `nv_revoke_gpu_mappings` | 正直な無し | |
| `spdm_crypto.c` の 14 個の false | 正直な無し | NVIDIA の版に従う |
| `spdm_crypto.c` の本体（912 行） | 動く | 呼ばれる道は Confidential Computing のものと頭のコメントが書き、`os_cc_enabled` は FALSE（`os.c:41`）。製品で走らない可能性がある。未確認 |

### `ports/mesa` の接着（全部読んだ）

| 箇所 | 分類 | 内容 |
|---|---|---|
| `nocturne_gl.c` の本体 | 動く | GL の入口と memory winsys |
| `mem_displaytarget_unmap`、`mem_displaytarget_display`、`mem_destroy`、`drawable_flush_front` | 何もしない | メモリーの面なので、ここは何もしないのが正しい |
| `ngl_scanout_*` の `#else` L631-649 | 死んだ枝 | 組まれない |
| ES のマイナー版 L313 | 決まった値 | 0 |
| antialias L285 | 決まった値 | false |
| `held_lines`、`log_free` L127-135 | 動く | 複数の libc の帰結（S2） |
| `nocturne_os.c` の `os_read_file` | 動く | |
| `nocturne_os.c` の `os_mkdir` | 動く | mode を捨てる。権限が無いので正しい |
| `nocturne_os.c` の `os_file_create_unique` L37-45 | 嘘の余地 | 確かめてから開くので一意でない。文書の排他の口があるのに使わない |

### 他の `ports`

| 場所 | 分類 | 内容 |
|---|---|---|
| `ports/lexbor/memory.c` | 動く | allocator の口 |
| `ports/wabt/wasm_rt_nocturne.c`、`wasm_rt_symbols.c`、`include/wasm-rt-*` | 動く | wasm2c のネイティブの土台。頭と宣言を読んだ |
| `ports/wabt/interp-x64.cc` | 動く | 上流に無い自前の JIT。頭だけ読んだ |
| `ports/wabt/include/wabt/config.h` | 動く | 上流の構成の口 |
| `ports/tcc/runmain.c` | 動く | 二つ目の libc を起こす（S2） |
| `ports/expat/expat_config.h`、`ports/freetype/*` | 動く | 上流が認めた構成の口。偽物ではない |
| `ports/mesa/generate.py`、`scripts/link_unit.py` | 動く | 偽物ではない |
| `ports/virgl` | ホストの DLL の修理 | ゲストの移植でない。置き場の問題だけ |
| `ports/llvm` の `Support/Nocturne/{Signals,DynamicLibrary,Watchdog,COM}.inc` | 未読 | 中身に空の関数がある可能性がある。読んでいない |
| `ports/go`、`ports/rust` の差分 | 未読 | 見出しと `README` だけ。`os/signal.Notify` は何も届けないと `README` が書く。正直な無しである |

### 偽の環境で組むもの

| 場所 | 定義 | 内容 |
|---|---|---|
| `nvrm.mk:66`、`nvkms.mk:67` | `-DNV_BSD` | 版の文字列を得るためだけ |
| `nvrm.mk:67` | `-DPORT_IS_KERNEL_BUILD=1` | プログラムなのにカーネルの組み |
| `uvm.mk` | `-DNV_KERNEL_INTERFACE_LAYER` | カーネルの層のふり |
| `nvrm.mk:81`、`nvkms.mk:83`、`uvm.mk:53` | `-mno-red-zone` | カーネルの習慣 |
| `Makefile:198` | `-D_FILE_OFFSET_BITS=64`、`-D_ISOC11_SOURCE` | glibc の機能の旗。我々の libc には効かない見込み。未確認 |
| `Makefile:134` | `-DBR_USE_UNIX_TIME=1` | BearSSL の `time()` の選択。名は UNIX だが ISO C の `time` で、問題でない |
| `third_party/nvidia-open/kernel-open/nvidia-uvm` の 40 ファイル | `NVRM_NOCTURNE` | Linux の見出しの読込みを外す |

何もしない lock は見つけていない。`os_alloc_spinlock` は mutex で、働く。読んだ範囲での結論である。

## 5. 残してよいもの

### 構造が正しいと判断したもの

| もの | 理由 |
|---|---|
| `--start-lib` と `--gc-sections` の結合の方法 | 小さなプログラムを小さく保つ。庫に切っても同じ方法が使える。`Makefile:315` のコメントだけ古い |
| `sys.c` の一呼出し一関数の包みの形 | 薄く、失敗は `n_result` で返り、`errno` に触らない |
| `errno.h` の写像 | ISO C++ の面に要る。独自の値を増やさない |
| `user/user.ld`、`user/image.map`、`driver_image.c` | 像の入口と表の形が正しい。記号を外へ出さない |
| `vulkan.c` | 像が無いときに Vulkan の結果で正直に失敗する。一つの像の問題は F3 |
| `scripts/build.py` | 46 行で、入口だけを持つ |
| `scripts/link_unit.py` | 弱い参照が NULL になる事故を組みで拒む |
| `scripts/layout.py` の表の形 | 古い出力の問題（S13）を除く |
| `n_pci_*`、`n_dma_*` の能力と claim の形 | 権利は能力の handle で渡し、自分の機能だけ書ける。画面の口の名は S7 |
| 装置ごとの `nvrm` プロセス | 故障を隔て、数が増えても呼ぶ側が変わらない |
| `os_stubs.c`、`os_ioctl.c`、`rm_api.c`、`os_caps.c`、`os_acpi.c`、`nvlink_os.c` の形 | NVIDIA が定めた `os-interface.h` が正しい継ぎ目である。未対応を未対応と言う |
| `nvrm_serve.c` の客ごとのスレッドと arena の読み替え | 番地を検査し、送り手の身元を毎回照合する（`:158-166`） |
| `nvkms_kapi_rm.c` | KAPI の表に足りないものを、NVKMS の旗で組んで私的な構造を一か所に閉じ込める |
| `display.c` が KAPI の唯一の客である形 | 中身は S9 |
| `nocturne_gl.c` の memory winsys、`ports/mesa/generate.py`、`nvk.mk` の単位の結合 | 働きと置き場が合う |
| `nvk_image.c`（Mesa の語を除く） | 像の表の実装で、余分な層が無い |
| `services.c`、`settings.c`、`language.c`、`keymap.c`、`audio.c`、`gui.c` | 持ち主のサービスや窓の口で、方針を持たない。読み手の無い分だけ消す |
| `malloc.c`、`threads.c`、`stdio.c` | 標準を native の handle の上に建てる。`threads.c` の頭の主張どおり POSIX の層でない |
| `ports/llvm` の `__nocturne__`、`ports/rust`、`ports/go` の族の扱い | UNIX の族に入らない。`posix_compat.h` の差分は「ここは使わない」と書くだけで、POSIX を再現していない |
| `ports/typescript` の overlay | 変更が 3 か所で最小である |
| `ports/wabt` の wasm2c の土台 | 記号の束ねと自前の trap は、ネイティブの形である |

### 構造は正しく、読みにくいだけのもの

- `ns_*` の接頭辞（`tls.c`）。意味が読めないが、層の問題ではない。
- `media*.c`、`http2.c` の詰めた書き方。読みにくいが、層の問題ではない。
- `display.c` と `nvkms_kapi_rm.c` の長いコメント。経緯に近いものが混ざるが、事実の記録として働く。
- コメントの言語の混在（英語と日本語）。
- 渡すだけの関数の大半（S17）。
- `nvrm_os.h` の共有の宣言。一枚で大きいが、一つのプログラムの中なので許せる。

## 6. 集計

数え方は `wc -l`。空行とコメントを含む。「残す」は中身を変えないという意味で、置き場所は移す。「判断が要る」は消す候補で、決めるのはユーザーである。書き直しの行は読んだ範囲からの見積りで、10 行の単位の推定である。

| 部品 | 全行 | 残す | 書き直す | 消す | 判断が要る |
|---|---:|---:|---:|---:|---:|
| `user/libc` ISO C と実行時 | 8,128 | 8,128 | 0 | 0 | 0 |
| `user/libc` 包みとサービスの口 | 3,179 | 2,403 | 476 | 300 | 0 |
| `user/libc` 窓、字体、画像 | 7,478 | 6,900 | 500 | 78 | 0 |
| `user/libc` 媒体 | 5,193 | 5,160 | 0 | 33 | 0 |
| `user/libc` 網、TLS、HTTP | 4,568 | 4,470 | 0 | 98 | 0 |
| `user/libc` ブラウザーの保存と通信 | 6,979 | 6,950 | 0 | 29 | 0 |
| **`user/libc` 小計** | **35,525** | **34,011** | **976** | **538** | **0** |
| `user/include` | 5,327 | 4,698 | 629 | 0 | 0 |
| `user/` 直下の結合の台本 | 59 | 56 | 0 | 3 | 0 |
| 小さな道具 39 本 | 1,356 | 0 | 0 | 0 | 1,356 |
| `user/apps/sh.c` | 882 | 0 | 0 | 0 | 882 |
| `syscallfuzz.c`、`cppdemo` | 851 | 851 | 0 | 0 | 0 |
| `ports/nvidia` | 15,679 | 8,927 | 1,170 | 0 | 5,582 |
| `ports/mesa`（`generated` を除く） | 1,749 | 1,690 | 40 | 19 | 0 |
| `ports/mesa/generated` | 31,173 | 31,173 | 0 | 0 | 0 |
| `ports/wabt` | 2,010 | 2,010 | 0 | 0 | 0 |
| `ports/go` | 7,471 | 7,411 | 60 | 0 | 0 |
| `ports/rust`、`ports/llvm` | 9,079 | 9,079 | 0 | 0 | 0 |
| `ports/typescript` | 383 | 297 | 0 | 86 | 0 |
| `ports` の小さなもの（ffmpeg、lexbor、tcc、expat、freetype、virgl） | 914 | 904 | 0 | 10 | 0 |
| `Makefile` | 637 | 297 | 340 | 0 | 0 |
| `scripts/layout.py`、`build.py` | 184 | 154 | 30 | 0 | 0 |
| **合計** | **113,279** | **101,558** | **3,245** | **656** | **7,820** |

内訳と限界：

- `generated` を除いた合計は 82,106 行、残すは 70,385 行である。
- 残すのうち、置き場所が変わるのは約 2 万 9 千行である（libc の ISO C 以外 25,883 行、見出し 2,838 行）。
- 「残す」は良いと確かめた意味ではない。次のものは中身を読まず、置き場所の判断だけで残すに数えた：`user/libc` の媒体 5,193、保存 6,979、網 4,568、`ui.c` 3,584、`ports` の差分の約 1 万 3 千行、`ports/mesa/generated`。
- `ports/nvidia` の「判断が要る」は UVM の 5,436 行と、`libnvrm.c` ほかの 146 行である。
- `ports/nvidia` の行数は `*.c`、`*.h`、`*.mk`、`include/*.h` で、`README.md` を含まない。
- 表に無い分：`third_party/nvidia-open/kernel-open/nvidia-uvm` の 154,123 行（判断が要る）。カーネルの変更（`struct n_launch`、`SYS_UDP_*`、`SYS_TCP_*`、OS の像）は他の監査の範囲である。
- 新しく書く分は別で、見積りは次のとおり。ドライバーの庫 約 800 行（大部分は移動）、OS の像 約 500 行、ネイティブの網の口 約 300 行、ABI の生成器 約 100 行。

## 7. 依存の順序

先に変えないと変えられないものを、上から順に書く。

| 順 | 変えるもの | 所見 | 先に要るもの | 理由 |
|---|---|---|---|---|
| 0 | ユーザーの判断：UVM を残すか。端末と道具を製品に残すか | S4、S6 | なし | 消す範囲と S7 の大きさが変わる |
| 1 | 読み手の無いコードと死んだ枝を消す。`ports/lexbor/include/memory.h`、`ports/ffmpeg/include/assert.h`、`ports/typescript/nocturne.patch`、`user/cxx.ld` | S17、節 4 | なし | 依存が無く、後の移動が軽くなる |
| 2 | 客の無い互換を消す：`Protocol1`、`SYS_ENTRY_INFO`、`errno` の古い記号 | S8、S12 | なし | 後の ABI の変更で気にせず済む |
| 3 | 偽の境界と偽の定義を消す | S10 | なし | `nvkms_os.c` を S7 で動かす前に薄くする |
| 4 | ABI の写しを生成にする | S16 | なし | S4、S5、S2 が ABI を変える。手の写しのまま変えると Go と Rust が壊れる |
| 5 | Makefile を部品に割る。生成物の規則を揃え、古い出力を媒体から外す | S13、S15 | なし | S1 で部品が動く前に、部品ごとの旗を持てるようにする |
| 6 | `nocturne.h` を割り、私的な見出しを部品へ移す | S11、S12 | 5 | S1 の前に include を動かす。全体ビルドになるが、理由にしない |
| 7 | libc を切る。順は媒体、ブラウザーの保存、網の上の層、窓の部品 | S1 | 5、6 | 依存は上向きだけなので、一番上から外す。`parallel.h` の段と `font.c` の web の語をここで移す |
| 8 | 字体の engine を一つにする。窓の部品から Web の設定を外す | S17 | 7 | 22 本の重複が減る。字体の全体の錠もここで見直す |
| 9 | TLS を一つにする | S3 | 7、tcc の確かめ | 番号の配り先が各部品へ分かれた後の方が直しやすい |
| 10 | 起動の ABI を直す：`environment`、`terminals`、`interrupts`、`#!`、`find_program` | S4 | 0、4、`programs` の口 | Go、Rust、`sh`、`term` が同時に動く |
| 11 | 網の口をネイティブにする | S5 | 4 | 呼ぶ側は `tls.c`、`network.c`、`timesync.c`、Go の差分 |
| 12 | OS の像で実行時を一つにする。複数の libc を消す | S2 | 7、9、F3、F4 | 実行時と包みの境界が決まらないと、像の表を決められない |
| 13 | ドライバーの庫を作り、三重の土台を一つにする | S7 | 0、3、7 | 庫の置き方は S1 の結果に従う。二枚目のドライバーの前に行う |
| 14 | `display.c` を分け、方針をサービスへ返す | S9 | 13、F10、F5 | 出力ごとの形を先に決める |
| 15 | 移植の方式を揃える | S14 | なし | いつでもできるが、1 から 13 が `third_party` の手入れを動かすので、後の方が無駄が無い |

並べてよいもの：

- 1、2、3、4 は互いに依存しない。
- 10 と 11 は 4 の後なら並べられる。
- 13 は 7 の窓の部品の切り分けを待たない。実行時と包みの境界が決まれば始められる。

他の監査との境：

- `struct n_launch`、`SYS_TCP_*`、`SYS_UDP_*`、`n_pci_screen_*`、入口の像はカーネルの監査の範囲で、順序を合わせる必要がある。
- `n_pci_give_screen` の方針の境と `programs` の口はサービスの監査の範囲である。
