# インストールの調べ：サービスと台帳の道（サブエージェントの報告の全文）

モデルは Sonnet 5.5。判定は [install-audit-20261102.md](install-audit-20261102.md) にある。

## 依頼文

D:\Programes\llm vibe slop os は Nocturne という独自の OS のリポジトリです。AGENTS.md と .claude/rules/map.md を先に読んでください。

読むだけの仕事です。ファイルを変えず、ビルドも QEMU も走らせず、コミットもしません。

問い：OS を組んだ後に、第三者のプログラムが OS に加わるとき、今のコードで何が起き、何ができず、何が OS の組みの時点で決まっているか。特に次を、コードの事実として調べてください。

1. サービスの宣言：user/apps/services.c の declared[] と関連の型（common/services_abi.h、common/authority_abi.h）。宣言は誰がいつ足せるか。実行時に足す口、消す口、利用者ごとのサービス、ログイン時に起こすもの、要るときに起こすもの、依存、失敗時の方針、能力の渡し方。組み込みでないプログラムがサービスを持てるか。
2. プログラムの台帳：user/apps/programs.c。台帳の中身はどこから来るか（ファイル、表、組みの生成物）。実行時に足す・消す口。文書の種類ごとの既定の登録。印、開始メニュー、設定の窓の「アプリ」の頁（user/apps/settings.c）との関係。
3. 媒体への入り方：scripts/layout.py、rootfs/、build/c.list の作られ方、Makefile の DISK_FILES。C:\Programs\<アプリ> は何に使われているか。
4. C: の権限：kernel/src/fs/store.c のパスの権限。誰が C:\Programs や C:\Nocturne に書けるか。起動時に検査や署名はあるか。
5. プログラムの身元：kernel/src/sys/authority.c の identity_for_child、store_path_number。プログラムの番号はパスから決まる。移動・更新・消去でプログラムの設定（user/apps/settingsd.c の N_SCOPE_PROGRAM）と私的な領域（N_CAP_STORE_PRIVATE、fs/document.c）に何が起きるか。消した後に残るものは何か。
6. 設定の宣言：settings_table.h はプログラムごとの宣言を実行時に受けるか。第三者のプログラムが自分の設定を持てるか。

書き方：
- 日本語で書く。識別子とパスは元の表記。一文に一つのことを書く。結論から書く。
- 主張ごとに file_path:line を付ける。読んだ原文で確かめた事実だけを「事実」と書き、推測は「推測」と書く。確かめていないことは確かめていないと書く。
- 「ある」「無い」は、探した語と場所を添える（無いことの根拠）。
- 良し悪しの判定より、何がどうなっているかを詳しく書く。
- 最後の返事が報告の全文です。ファイルには書きません。

## 報告

# 調査報告：OS を組んだ後に第三者のプログラムが加わるとき

読むだけで調べた。ファイルは変えず、ビルドも QEMU も走らせていない。動かして確かめたことは一つも無い。以下の「事実」は読んだ原文で確かめたもの。「推測」は読んだことからの見込み。

## 結論

- 管理者の宣言の表 `declared[]` は C の const 配列で、`services` のバイナリに組み込まれる。実行時に足す口も消す口も無い。組み込みでないプログラムが管理者の管理下のサービスになる道は、今のコードに無い。
- 組み込みでないプログラムは `n_service_listen` で名前を申し込める。ただし起動、依存、再起動の管理は付かない。
- プログラムの台帳（`programs`）は、システムの表、`C:\Programs` の下の登録、利用者ごとの選択の三つでできている。登録と消去の口は実行時にある。口を使えるのは admin 以上。
- `C:\Programs` に書けるのは admin 以上。`C:\Nocturne` に書けるのは system 級だけ。
- 起動時の署名と検査は無い。ELF の形の検査だけがある。
- プログラムの番号は、起こしたときのパス文字列の FNV-1a ハッシュ。移動と上書きでは変わり方が違う。消しても、設定の値は消えない。
- `N_SCOPE_PROGRAM` の設定は、第三者のプログラムも実行時の宣言で持てる。ただし宣言はメモリー上だけで、設定の窓には出ない。
- 私的な領域（`N_CAP_STORE_PRIVATE`）はプログラムに結び付いていない。第三者のプログラムが自分用に持つ口は無い。

## 1. サービスの宣言

### 表と組み込み

- 事実：宣言の表は `static const struct declared_service declared[]`（`user/apps/services.c:60`）。ファイル頭の注釈も「プログラムに組み込まれている」と書く（`user/apps/services.c:3-4`）。
- 事実：項目の型は `struct declared_service`（`user/apps/services.c:38-52`）。持つ項目は次のとおり。
  - `name`
  - `program`
  - `cls`
  - `scope`
  - `at_boot`
  - `optional`
  - `console`
  - `caps`
  - `needs`
  - `on_failure`
  - `clients_from`
  - `function_matches`
- 事実：管理者は表を読むだけで、足す口も消す口も持たない。`declared[]` を変える関数は無い。管理者の口の操作は LIST、STATUS、RESTART、STOP、WATCH だけで、足すものも消すものも無い（`common/services_abi.h:21-27`、`user/apps/services.c:684-701`）。探した語は `declared`、`N_SERVICES_`。
- 事実：`declared[]` に足せるのは、`services` を組み直せる者だけ。

### 起こす時と依存

- 事実：`at_boot` が真なら要求を待たずに起こす（`user/apps/services.c:226`）。偽なら、要求が来たときに起こす。
  - 要求が来ると `in->wanted = true` にする（`user/apps/services.c:743`）。
  - `programs` は `at_boot` が無く、要るときに起こされる（`user/apps/services.c:76-77`）。
  - 仕事を終えて結果 0 で終わったあと、待っている要求があれば起こし直す（`user/apps/services.c:459-463`）。
- 事実：依存は `needs`。起動の順は `visit` が依存から計算する（`user/apps/services.c:279-300`）。
  - 循環と、宣言に無い名前は「壊れた実例」になり、`give_up` で FAILED になる（`user/apps/services.c:418-421`）。
  - 依存の準備は `n_service_ready` の知らせで RUNNING になって知る（`user/apps/services.c:485-486`）。
  - `optional` で媒体に無い（ABSENT）か、FAILED の相手は、満たされたとみなす（`user/apps/services.c:312-322`）。
- 事実：失敗時の方針は `struct failure_rule {restarts, interval_ms, healthy_ms}`（`user/apps/services.c:32-36`）。
  - 落ちたら BACKOFF で間隔を置いて起こし直す。
  - 回数を超えたら `give_up` で FAILED になる。
  - 健康に動いた時間を超えたら、回数を数え直す（`user/apps/services.c:465-475`）。
  - 結果 0 で終わる場合、または管理者が頼んだ終わりは失敗に数えない（`user/apps/services.c:459`）。
- 事実：FAILED から戻すには RESTART が要り、それは admin 以上だけができる（`user/apps/services.c:691-694`）。

### 利用者ごと、機械ごと

- 事実：宣言の `scope` は `SCOPE_MACHINE`（機械に一つ）と `SCOPE_FUNCTION`（PCI の機能ごとに一つ）の二つだけ（`user/apps/services.c:27-30`）。利用者ごと、セッションごとの宣言の scope は無い。
- 事実：宣言のサービスは `identity = {.user = 0, .session = 0, .cls = d->cls}` で起こされる（`user/apps/services.c:361`）。宣言のサービスは常に利用者 0 の身元になる。
- 事実：利用者ごと、セッションごとに引ける名前は、申し込まれた名前（bound name）だけ（`user/apps/services.c:501-515`）。
  - 同じ利用者とセッションの中でだけ引ける。
  - ただし `from.user == 0` か `cls >= N_CLASS_SYSTEM` なら機械全体で引ける（`user/apps/services.c:539`）。admin 級だけでは機械全体にならない。
  - 宣言の名前と管理者の名前は申し込めない（`name_is_ours`、`user/apps/services.c:517-519`、536）。
  - 機械全体の名前が先に引かれるので、利用者は機械全体の名前を自分の範囲で隠せない（`user/apps/services.c:505-515`）。

### ログイン時

- 事実：ログイン時に何かを起こす仕組みは、見つけた範囲で `session.c` の二つだけ。
  - 利用者のシェル（`C:\Nocturne\System\shell`）か `sh` を起こす。
  - 「自動起動」の一覧は見つからなかった。探した語は `startup`、`autostart`、`Startup`（`user/apps/session.c`）。
- 事実：`session.c` の `n_app_launch` は、起こすときに利用者の身元をセッションに渡す（`user/apps/session.c:155`、99）。

### 能力の渡し方

- 事実：宣言の `caps` の一つずつを、根の能力から `n_cap_derive(root_cap, d->caps[i])` で切り出し、起こすときの handle に入れる。管理者が根の能力を持つ（`user/apps/services.c:352-357`、815）。
- 事実：カーネルは能力を kind だけで持つ。根は全部を、ほかの能力は同じ種類だけを切り出せる（`kernel/src/sys/authority.c:92-102`）。
- 事実：`N_CAP_COUNT` は 13 種類（`common/authority_abi.h:15-30`）。第三者が新しい種類を足す口は無い。種類の追加は ABI の変更。
- 事実：能力は handle なので、持つ者が起こすときの `handles` に入れれば渡せる（`kernel/src/sys/syscall.c:858-863`）。

### 組み込みでないプログラムのサービス

- 事実：`n_service_listen`（`user/libc/sys.c:395`）は `N_SERVICE_BIND` へ名前を送る。管理者の `bind_serve` が受ける（`user/apps/services.c:524-561`）。
  - 管理者は名前と、受け口へ送る側の channel の端を受け取る。
  - 宣言の名前、管理者の名前、既存の名前は断る。
  - 受け口の端が全部閉じると、名前は消える（`user/apps/services.c:564-570`）。
- 事実：申し込まれた名前には管理者の管理が付かない。
  - 起動の順、`needs`、`on_failure`、`at_boot` が無い。
  - 要求は `mouth_put` で b->mouth に積まれ、申し込んだプログラムが動いている間だけ受けられる（`user/apps/services.c:748-753`）。
  - 申し込んだ者を起こす側の処理は無い。
  - `N_SERVICES_LIST` の `snapshot` は `instances`（宣言の実例）だけを返し、bound name は含まない（`user/apps/services.c:597-613`）。
- 推測：第三者のプログラムが動いていないとき、その名前への要求は宣言の名前と違って起動に結び付かない。コードは「申し込みが無い名前は何も起こさず channel を閉じる」（`user/apps/services.c:756`）。
- 事実：settings の窓のサービスの頁は `N_SERVICE_MANAGER` から実例を読む（`user/apps/settings.c:328`）。bound name はそこに出ない。

### 古い注釈

- 事実：`user/apps/services.c:107` の「プログラムの番号は表の位置から。0 は宣言に無いプログラム。」は、今のコードと違う。
  - 番号はカーネルがパスから決める（`kernel/src/sys/authority.c:51`、`kernel/src/fs/store.c:659-677`）。
  - この行の下に続きのコードは無く、残った注釈。

## 2. プログラムの台帳

### 台帳の中身の出どころ

- 事実：台帳の中身は二つから来る。
  - システムのプログラム：`programs.c` の `system_programs[]` という const 表（`user/apps/programs.c:20-46`）。ID は表の位置 + 1。`system_load` が `on_medium`（媒体にある文書か）を確かめ、無いものは台帳に入れない（`user/apps/programs.c:260-276`）。
  - 登録のプログラム：私的な領域 `N_REGION_PROGRAMS` の document に、`RECORD_PROGRAM`、`RECORD_CLAIM`、`RECORD_CHOICE` の三種の記録で保存される（`user/apps/programs.c:50-56`、221-244、278-306）。
- 事実：`system_programs[]` が名指すパスは二系統ある。
  - `C:\Nocturne\System\...`（term、files、settings、sysmon、js）。
  - `C:\Programs\<フォルダー>\<名前>`（browser、notepad、paint など）。
  - このフォルダー名は `scripts/layout.py` の `APPS`（`scripts/layout.py:32-37`）と一致させてある。
- 事実：登録のプログラムの保存は「`N_PROGRAM_SYSTEM` でないもの」だけ（`user/apps/programs.c:223-225`）。
  - 起動時の `table_load` は保存済みの `RECORD_PROGRAM` を `on_medium` で確かめず、そのまま `program_add` する（`user/apps/programs.c:291-293`）。
  - そのため、媒体から消えた登録のプログラムも台帳に残る。
  - 推測：台帳が残るだけで、起こすときは `n_app_launch` が失敗する。
  - `list_pack` も `on_medium` を確かめない（`user/apps/programs.c:419-441`）。

### 実行時の口

- 事実：登録は `N_PROGRAMS_REGISTER`（`user/apps/programs.c:484-487`）。条件は次のとおり。
  - `admin`（`peer->cls >= N_CLASS_ADMIN`）。
  - パスが `C:\Programs\` で始まる（`strncasecmp(t->path, PROGRAMS, ...)`、`user/apps/programs.c:324`）。
  - `on_medium(path)` が真（文書として存在する）（`user/apps/programs.c:325`）。
  - 同じパスが台帳に無い（`user/apps/programs.c:326`）。
  - 番号は `next_id` で、消しても使い回さない（`user/apps/programs.c:83`、327-336）。
- 事実：消去は `N_PROGRAMS_REMOVE`（admin 以上）。`N_PROGRAM_SYSTEM` は消せない（`user/apps/programs.c:356-378`）。
  - 消す物は、そのプログラムの記録、その path を選んだ全利用者の選択、そのプログラムの種類の組。
  - 消した後に台帳を保存し直す（`table_save`）。
- 事実：種類の宣言は `N_PROGRAMS_CLAIM`（admin 以上）。選択は `N_PROGRAMS_CHOOSE`（誰でも、自分の分だけ。`user == 0` は拒否）（`user/apps/programs.c:403-410`、488）。
- 事実：どの種類を開くかは次の順で決まる（`program_for`、`user/apps/programs.c:387-393`）。
  - その利用者の選択。
  - 種類を名乗った最初のプログラム。
  - 何でも開く（`"*"`）最初のプログラム。

### 窓と UI との関係

- 事実：開始メニューと印は、シェルが `n_programs_list` を呼んで読む（`user/apps/shell.c:137-166`）。
  - `N_PROGRAM_MENU` と `N_PROGRAM_DESKTOP` の旗で振り分ける。
  - 登録のときに付けられる旗は `SHOWN`（メニューと印）だけで、`q->flags & SHOWN` で切る（`user/apps/programs.c:329`）。
  - `N_PROGRAM_SYSTEM` は登録で付かない。
- 事実：起こすのは `n_program_run` で、`n_app_launch(p->path, &launch)`。`identity` も `handles` も指定しない（`user/libc/services.c:237-245`）。
  - 起こしたプログラムは、起こした者の身元を継ぐ（利用者のシェルなら利用者の級）。能力は渡らない。
- 事実：設定の窓の「アプリ」の頁は、台帳の一覧と種類の組から組む（`user/apps/settings.c:1114-1255`）。
  - 頁には種類ごとの既定の選択、登録済みの一覧（`N_PROGRAM_SYSTEM` は除く）、名前とパスと種類を入れる追加の欄がある。
  - 追加は `n_program_register` で足し、アイコンは `N_ICON_GENERIC`、旗は `N_PROGRAM_MENU` 固定（`user/apps/settings.c:1181`）。
  - 種類は `n_program_claim` で別に足す（`user/apps/settings.c:1183-1189`）。
- 事実：登録のプログラムのアイコンは、窓の経路では `N_ICON_GENERIC` 固定。`n_program_register` の `icon` と `flags` を使う呼び出しは、`settings.c` の一箇所だけ。探した語は `n_program_register`（`user/apps` と `user/libc`）。
- 事実：登録を足せる UI は、設定の窓だけ。インストーラーや、プログラム自身が台帳へ足す仕組みは見つからなかった。

## 3. 媒体への入り方

- 事実：媒体に入るものは `scripts/layout.py` が作る一覧（`build/c.list`）が決める。一行は「C: のパス、ホストのファイルかフォルダー、属性」（`scripts/layout.py:130-131`）。`build/host/store.exe fill` がこれを詰める（`scripts/layout.py:7`、`Makefile:587-591`）。
- 事実：詰める先は次のとおり（`scripts/layout.py:76-106`）。
  - `C:\Nocturne` は先に「!」（中身ごと消す）で空にしてから、`rootfs/Nocturne`、`Logs`、`Licenses`、`SDK`、`Certificates\ca-bundle.crt`、`System` を置く。
  - `build/programs/` の中の各ファイルのうち、`APPS` に名前があるもの（browser、notepad、paint、calc、clock、piano、player、mediaplayer、snake、tetris、mines、mandel、cube、life、agent）は `C:\Programs\<フォルダー>\<名前>` へ。
  - それ以外はすべて `C:\Nocturne\System\<名前>` へ。
  - `C:\Programs\<app>` は、`APPS` にあるものだけ「!」で消してから詰める。`APPS` に無い `C:\Programs` の中のフォルダーは、`fill` では消えない。
  - 新しい利用者の元（`C:\Nocturne\NewUser`）は `rootfs/NewUser`、音楽、`diskfiles/` から作る。
  - `--system` なしなら最後に `C:\Users` の空のフォルダーが付く。
- 事実：`--system` の説明どおり、「システムの部分は丸ごと置き換え、前の fill が置いたものが先に消える。ディスクは利用者のファイルとアプリを残したまま、システムを更新できる」（`scripts/layout.py:14-17`）。
- 事実：`Makefile` の `DISK_FILES` は、媒体の依存を列挙している。`$(BUILD)/tests/testrun`、`$(APP_BINS)`、`$(GO_FILES)`、`$(RUST_FILES)`、`$(LLVM_FILES)`、`$(NVRM_FILES)`、`$(PROGRAMS)/tcc`、`$(LICENSE_FILES)`、`sdk.stamp`、`sounds.stamp`、`$(NVIDIA_FW_STATE)`、`rootfs` のファイル全部、`third_party/ca/ca-bundle.crt`（`Makefile:576-577`）。`qemu-image` と `image` はこれに依存する（`Makefile:587-591`）。`$(BUILD)/tests/testrun` は媒体には入れないが一緒に組むと注釈されている（`Makefile:575`）。
- 事実：`rootfs/` には `NewUser/Documents/` と `Nocturne/{Agent,Fonts,Licenses,SDK,Web,Welcome.txt}` がある（`ls rootfs`）。
- 事実：`C:\Programs\<アプリ>` は、次の三つに使われている。
  - `scripts/layout.py` が `APPS` のアプリを置く場所。
  - `programs.c` の `system_programs[]` の一部の起動パス。
  - `programs.c` の登録（`N_PROGRAMS_REGISTER`）が許す唯一の場所（`user/apps/programs.c:324`）。
- 事実：`user/apps/sh.c:100-102` が `N_FOLDER_PROGRAMS` のフォルダーを列挙する。
- 事実：ビルド済みの媒体へ組み直さずに、実行中の OS へ後から第三者のプログラムを入れる機構（インストーラー、パッケージ、ダウンロード）は見つからなかった。探した語は `install`、`package` ではなく、台帳の口と `layout.py` の経路。確かめていないこと：`sh.c` のコマンドや `user/apps/*.ts` に、インストールに当たる処理があるか。

## 4. C: の権限

### パスの権限（`kernel/src/fs/store.c:585-610`）

事実：`place_allows(p, who, write)` は区域ごとに決める。

| 区域 | 読む | 書く |
|---|---|---|
| `AREA_SYSTEM`（`C:\Nocturne`） | 誰でも | `N_CLASS_SYSTEM` のみ |
| `AREA_PROGRAMS`（`C:\Programs`） | 誰でも | admin 以上 |
| `AREA_HOME`（`C:\Users\<名前>`） | admin か持ち主 | admin か持ち主 |
| `AREA_TOP`（C:\ の直下、`C:\Users` の直下の新しい名前） | 誰でも | admin 以上 |
| `AREA_OTHER` | admin 以上 | admin 以上 |

- 事実：区域は `C:\` の直下の項目から決まる（`area_step`、`kernel/src/fs/store.c:586-597`）。`store.system`、`store.programs`、`store.users` の id と比べる。`C:\Users` の直下は `AREA_HOME` か `AREA_TOP`。
- 事実：`C:\Nocturne`、`C:\Programs`、`C:\Users` の三つの項目そのものを消す、移すのは、system 級だけ（`place_may_change`、`kernel/src/fs/store.c:703-709`）。
- 事実：名前変更と移動は `store_entry_move` が両側に `place_may_change` を掛ける（`kernel/src/fs/store.c:949`、953）。`C:\Programs` の中なら admin 以上が要る。
- 事実：`STORE_PRIVATE` 属性のエントリーは、`user` 側からは `N_ERR_NOT_FOUND` になる（`kernel/src/fs/store.c:638`、689）。

### 起動時の検査

- 事実：署名と検査は無い。`proc_launch` が見るのは ELF の形だけ。
  - ELF のマジック、64 ビット、`machine == 0x3E`（x86-64）、`type == 2`（実行形式）。
  - `phentsize`、`phoff`、`phnum` がファイルの中に収まるか（`kernel/src/sys/proc.c:184-187`）。
  - 形が合わなければ `N_ERR_NOT_PROGRAM`。
- 探した語は `signature`、`sha256`、`verify`、`elf_verify`（`kernel/src` 全体）。ファイルの内容のハッシュや署名の検査に当たる呼び出しは見つからなかった。`signature` の一致は ACPI、AHCI、USB、hibernate などで、プログラムの検査とは別。
- 事実：`proc_launch` は `store_document_open(path, N_DOCUMENT_READ, false, &doc)` で開く（`kernel/src/sys/proc.c:177`）。第三引数 `user = false` は、パスの権限を確かめない。`place_find` は `user` が偽のとき `who = NULL` で `place_allows` を呼ばない（`kernel/src/fs/store.c:624`、640）。
  - つまり、起こす側がそのファイルを読める必要は無い。`AREA_OTHER` や他の利用者の `C:\Users\<名前>\...` にある ELF も、パスを知っていれば起こせる。
  - 起こした子の級は起こした者を超えられない（`kernel/src/sys/authority.c:42`）。
  - 推測：読めない文書を起こせることは、プログラムの中身を読み出す道ではない（子の ELF の内容を親は読めない）。ただし、他人の `C:\Users` の中のプログラムを起こせる点は、ここで確かめた事実の帰結。

## 5. プログラムの身元

### 番号の決め方

- 事実：`identity_for_child` が子の `child->program` を決める（`kernel/src/sys/authority.c:34-52`）。
  - まず `child->program = self->program` で親の番号を写す。
  - `N_LAUNCH_SAME_PROGRAM` が立っていれば、そのまま親の番号で終わる（子の部品が親の設定を読むため。`common/abi.h:295`）。
  - そうでなければ `store_path_number(path, &child->program)` で、起こすパスから番号を決める。
- 事実：`wanted.program != 0` の要求は `N_ERR_INVALID` で断る（`kernel/src/sys/authority.c:41`）。番号は呼ぶ側が決められない。
- 事実：`store_path_number` は、パスを名前ごとに分け、大文字小文字を畳み（`name_fold`）、名前の間に区切りの 0 バイトを挟んで、FNV-1a 64 ビットで混ぜる（`kernel/src/fs/store.c:659-677`）。0 は 1 にする。
  - 文書の ID や中身は使わない。パス文字列だけで決まる。
  - ファイルが存在するかは確かめない。
- 推測：FNV-1a は暗号用でない。64 ビットなので偶然の衝突は考えにくいが、意図して衝突するパスを作ることは理論上できる。衝突の実例は確かめていない。
- 事実：`path_parse` が `.` や `..` をどう扱うかは読んでいない。確かめていないこと：同じファイルを指す別の書き方のパスが同じ番号になるか。起こす文字列が、そのまま `proc_launch` と `store_path_number` の両方に渡る（`kernel/src/sys/syscall.c:821`、882）ことは事実。
- 事実：番号の 0 は最初のプロセスだけ（`common/authority_abi.h:12`）。

### 移動、更新、消去で起きること

- 事実：番号がパスで決まるので、次のようになる。
  - 上書き更新（同じパスに新しいバイナリー）：番号は変わらない。設定の値と登録はそのまま続く。
  - 移動、名前変更：番号が変わる。新しい番号のプログラムは、古い番号の設定の値を読めない。
  - 大文字小文字の違いだけのパス：番号は同じ。
- 事実：台帳（`programs.c`）の側では、プログラムの識別は `id`（登録順の番号）と `path` の文字列の両方で、移動に追従しない。
  - `program_at_path` は `strcasecmp` で台帳の `path` と比べる（`user/apps/programs.c:119-123`）。
  - 選択（`choice`）は `path` で結ぶ（`user/apps/programs.c:70-73`、390）。
  - 移動の通知を台帳が受ける仕組みは見つからなかった。台帳の登録は古いパスを指したまま。
  - 確かめていないこと：ファイルの変更を `fs/watch.c` 経由で台帳が見張っているか。`programs.c` に `watch` の語は見つからなかった。
- 事実：消去しても、設定の値は消えない。
  - `settingsd.c` で値を消す関数 `value_remove` の呼び出しは、保存に失敗したときの巻き戻し一箇所だけ（`user/apps/settingsd.c:298-304`、690）。
  - 設定のサービスの操作に、削除の操作は無い（`enum n_settings_op`、`common/settings_abi.h:251-264`）。
  - プログラムを消す側（`programs.c` の `program_remove`）は、設定のサービスを呼ばない（`user/apps/programs.c:356-378`）。
  - `store_save` は値の表の全部を書く（`user/apps/settingsd.c:452-460`）。だから消えたプログラムの値も保存され続ける。
  - 同じパスに別のプログラムを置くと、同じ番号になるので、前のプログラムの値を読める（推測。上の番号の決め方から出る）。
- 事実：消した後に残るものは次のとおり。
  - 設定の値：`N_SCOPE_PROGRAM` の値は、`(domain, key, device, user, program)` の組で保存済みのまま。
  - 台帳の `next_id`：消しても使い回さない（`user/apps/programs.c:48`）。
  - 台帳の種類の組と選択：`program_remove` が消す（`user/apps/programs.c:361-377`）。
  - ディスク上のファイル：台帳の消去は台帳だけで、プログラムのファイルは消さない（`program_remove` は `n_*` の消去を呼ばない）。
  - 私的な領域の document（下記）。

### 私的な領域（`N_CAP_STORE_PRIVATE`）

- 事実：`sys_store_private_open(cap, region, user, access)` は、`N_CAP_STORE_PRIVATE` の能力を持つ者だけが開ける（`kernel/src/sys/syscall.c:553-559`）。
- 事実：領域の名前は `store_region_name(user, region, name)` で、利用者の番号と領域の番号から決まる（`kernel/src/fs/document.c:739-740`）。プログラムの番号は入らない。
- 事実：領域は `STORE_ID_REGIONS` の中の document で、無ければ作る（`kernel/src/fs/document.c:735-765`）。`N_REGION_SETTINGS`、`N_REGION_USERS`、`N_REGION_SECRETS`、`N_REGION_PROGRAMS` の四つが定義済みで、`N_REGION_FIRST_FREE = 16`（`common/settings_abi.h:151-155`）。
- 事実：開く呼び出しは三箇所だけ。
  - `settingsd.c:421`（`N_REGION_SETTINGS`）。
  - `programs.c:533`（`N_REGION_PROGRAMS`）。
  - `accounts.c:366`（`N_REGION_USERS`）。
  - 探した語は `n_store_private_open`、`N_REGION_FIRST_FREE`（リポジトリ全体の `*.c`、`*.h`、`*.cpp`）。`N_REGION_SECRETS` を開く呼び出しも、`N_REGION_FIRST_FREE` 以上の番号を使う呼び出しも見つからなかった。
- 事実：能力を持つサービスは、`declared[]` の `CAPS(N_CAP_STORE_PRIVATE)` を持つ `accounts`、`settings`、`programs` の三つだけ（`user/apps/services.c:67-77`）。
- 推測：第三者のプログラムが自分用の私的な領域を持つ道は、今は無い。能力を宣言に足すには `services` の組み直しが要る。能力を持つサービスが能力を渡す（委譲）ことは handle なので可能だが、そうする仕組みは見つからなかった。
- 事実：領域は利用者の番号と領域の番号で決まり、プログラムの移動、更新、消去とは無関係に残る（プログラムの番号を鍵に使っていないため）。

### 参考：programs.c の ID

- 事実：台帳の `id`（`n_program_head.id`）は、`system_programs[]` の位置 + 1 と登録順の番号で、カーネルの身元の `program` 番号（パスのハッシュ）とは別のもの（`user/apps/programs.c:48`、264、327）。

## 6. 設定の宣言

### 宣言を受ける口

- 事実：`settings_table.h` は、カーネルの領域の鍵と、適用の無い鍵（`N_APPLY_NONE`）の静的な表で、ここにはプログラムごとの宣言は入らない。ファイル頭の注釈も「プログラムの鍵は、それぞれが `N_SETTINGS_DECLARE` で足す」と書く（`user/apps/settings_table.h:1-3`）。
- 事実：実行時の宣言は `N_SETTINGS_DECLARE` で受ける（`user/apps/settingsd.c:871-906`）。`decl_valid`（`user/apps/settingsd.c:189-215`）が名前、長さ、型、範囲、既定を確かめる。
- 事実：`N_SCOPE_PROGRAM` の宣言は、次の条件で通る。
  - `domain == N_DOMAIN_PROGRAM`（0xffffffff）と `scope == N_SCOPE_PROGRAM` は必ず対にする（`user/apps/settingsd.c:193`）。
  - `apply == N_APPLY_NONE` に限る（`user/apps/settingsd.c:194`）。
  - 宣言した者の `who->program` が 0 でない（0 は `N_ERR_DENIED`）（`user/apps/settingsd.c:883-888`）。
  - 宣言は `who->program` に結び付く（`known_add(&d, program)`）。
  - 宣言に級の条件は無い。USER 級のプログラムでも宣言できる。
- 事実：読み書きの権限は、鍵の `known` が `(domain, key, program)` で引かれることで本人に絞られる（`user/apps/settingsd.c:995-996`、550-567）。
  - `target` が `id->program = who->program`、`id->user = who->user` に書き換える。
  - 他のプログラムの鍵を読み書きする道は無い。
  - `may_write` はプログラムの scope なら true（`user/apps/settingsd.c:579`）。本人であることは `target` が保証する。
- 事実：`N_SETTINGS_LIST` は、プログラムの scope の鍵のうち、呼ぶ側のプログラムの番号と同じものだけを返す（`user/apps/settingsd.c:854`）。
- 事実：見張り（WATCH）も、プログラムの scope なら番号と利用者が同じ接続にだけ知らせる（`user/apps/settingsd.c:588`）。
- 事実：第三者のプログラムが自分の設定を持てるのは、次の条件を満たす場合。
  - 事実：libc が `n_setting_declare` を提供している（`user/libc/settings.c:251-258`）。
  - 事実：`user/libc/webprefs.c:355-364` と `user/apps/agent.c:160-173` が、すでにこれを使っている（`.domain = N_DOMAIN_PROGRAM` の宣言）。
  - 事実：宣言を足す口に、組み込みの条件は無い。プログラムが起動のたびに宣言を送る形。

### 宣言が残らないこと

- 事実：宣言（`knowns`）はメモリー上の配列で、保存しない（`store_save` は `values` と `trials` だけを書く。`user/apps/settingsd.c:452-460`）。`settings` サービスの再起動では、宣言が消える。各プログラムは再び宣言するまで `N_SETTING_UNKNOWN_KEY` を受ける（`user/apps/settingsd.c:997-998`）。
- 事実：値（`values`）は宣言とは別に保存される。宣言の無い値は、保存したまま未使用で残る。
- 事実：値を読むとき、`N_SETTINGS_GET` は宣言が無ければ `N_SETTING_UNKNOWN_KEY` で、保存された値があっても返さない（`user/apps/settingsd.c:997-998`）。

### 設定の窓

- 事実：設定の窓の汎用の頁は、`n_setting_list` で宣言を読み、`shown[i].d.domain == pages[page].domain` かつ `N_SETTING_DECL_CUSTOM` でないものを並べる（`user/apps/settings.c:1293`）。
- 事実：`settings.c` に `N_DOMAIN_PROGRAM` と `N_SCOPE_PROGRAM` の語は見つからなかった。
- 事実：設定の窓のプロセスのプログラム番号は、窓のパスから決まる。窓に見えるプログラムの scope の鍵は、設定の窓自身のものだけ（`settingsd.c:854`）。
- 推測：第三者のプログラムの設定は、設定の窓から見えない。プログラム自身が自分の窓で見せる前提の作り（`webprefs.c` と `agent.c` が同じ作り）。

### 領域の宣言の取り込み

- 事実：非プログラムの scope の宣言は、`N_CLASS_ADMIN` 以上で、`d.domain < N_DOMAIN_COUNT` のときだけ通る（`user/apps/settingsd.c:889-892`）。通ると、その接続が持ち主（`k->owner = c`）になり、保存済みの値が持ち主へ当て直される（`owner_catch_up`、`user/apps/settingsd.c:903-905`）。
- 事実：拒むのは `there->kernel` の鍵（`settings_table.h` の鍵）だけ（`user/apps/settingsd.c:893-897`）。`there` が非カーネルの鍵（例：`audio` がこの経路で宣言する鍵、`user/apps/audio.c:1111-1129`）なら、`known_add` が `knowns[at]->d = *d` で宣言を置き換える。
- 推測：admin 級のプログラムは、既存の `audio` や `display` のサービスの鍵を再宣言して持ち主になれる。コードの読みから出た見込みで、実行して確かめていない。第三者を admin 級で動かさなければ影響は出ない。

## 確かめていないこと

- 実機、QEMU で第三者のプログラムを足す実験。
- `path_parse` が `.`、`..`、重複する区切りをどう正規化するか。同じファイルの別の書き方のパスが同じ番号になるか。
- `sh.c` と `user/apps/*.ts` に、プログラムの登録や配置を手伝う処理があるか。
- ファイルの移動、消去を台帳が見張るか（`programs.c` に `watch` の語は見つからなかった）。
- `docs/` 内の文書の記述。コードだけを根拠にした。

## 読んだ主なファイル

- `D:\Programes\llm vibe slop os\user\apps\services.c`
- `D:\Programes\llm vibe slop os\common\services_abi.h`
- `D:\Programes\llm vibe slop os\common\authority_abi.h`
- `D:\Programes\llm vibe slop os\user\apps\programs.c`
- `D:\Programes\llm vibe slop os\user\apps\settings.c`（1112-1255 行）
- `D:\Programes\llm vibe slop os\user\apps\settingsd.c`
- `D:\Programes\llm vibe slop os\user\apps\settings_table.h`
- `D:\Programes\llm vibe slop os\common\settings_abi.h`
- `D:\Programes\llm vibe slop os\scripts\layout.py`
- `D:\Programes\llm vibe slop os\Makefile`（560-605 行）
- `D:\Programes\llm vibe slop os\kernel\src\sys\authority.c`
- `D:\Programes\llm vibe slop os\kernel\src\fs\store.c`（540-990 行）
- `D:\Programes\llm vibe slop os\kernel\src\fs\document.c`（715-770 行）
- `D:\Programes\llm vibe slop os\kernel\src\sys\proc.c`（150-240 行）
- `D:\Programes\llm vibe slop os\kernel\src\sys\syscall.c`（550-560、795-895 行）
- `D:\Programes\llm vibe slop os\user\libc\services.c`、`user\libc\settings.c`、`user\apps\shell.c`、`user\apps\session.c`（部分）
