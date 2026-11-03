# USB と記憶の driver の九回目の監査：記憶の driver

## 依頼文

Nocturne OS（独自の OS、カーネルは C、対象は実機）の記憶の driver の差分を、公式の文書と敵対的に照らしてください。返事と報告は日本語です。

作業場所：`D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64`（読むだけ。コードは変えない）。

材料：
- 差分：`git diff 9bea7571 HEAD -- kernel/src/dev/ahci.c kernel/src/dev/nvme.c`。周りのコードは今のファイルで読む。
- 文書の text（節番号を自分で確かめる）：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\` の ahci.txt（AHCI 1.3.1）、nvme24.txt、nvmepcie.txt、pcie6.txt。原本は `D:\Programes\OS DEV INFO`。
- カーネルの規則：`.claude/rules/kernel.md`。

範囲：差分の行が生んだ欠陥か、差分の行そのものと文書の差だけを挙げてください。差分の外の問題は、データを失うものだけを挙げてください。推測で疑わず、一件ごとにコードの場所と文書の節を確かめてから書いてください。`docs/` の下の他の監査の報告は読まないでください。Linux その他の OS のソースは参照しないでください。

報告：全文を `docs/records/kernel-core-20261101/audit-usbstorage-fix9-storage.md` に書いてください（先頭にこの依頼文）。各指摘の形：重さ（データを失う／当たれば壊れる／きれいさ）、場所（ファイル:行）、文書の節と日本語の要旨（原文を長く写さない）、起きる道筋、直し方の案。指摘が無ければ無いと書いてください。コミットはしないでください。最後は SubagentHandback で、件数と重さごとの内訳と報告のパスを返してください。

## 結論

指摘は 2 件である。

| 重さ | 件数 |
|---|---|
| データを失う | 0 |
| 当たれば壊れる | 0 |
| きれいさ | 2 |

照らした版は HEAD の `97e41771` である。行番号はこの版のものである。ビルドと実行はしていない。これは静的な確認である。

## 指摘

### 1. 別の装置への shutdown の待ちが、前の装置の RTD3E で決まる

- **重さ：** きれいさ。
- **場所：** `kernel/src/dev/nvme.c:555-562`、`kernel/src/dev/nvme.c:577`、`kernel/src/dev/nvme.c:999`、`kernel/src/dev/nvme.c:1011`。
- **文書：** NVMe Base 2.4 の 3.6.1。host は Identify Controller が報告する RTD3 Entry Latency を最短の待ちとすることが推奨される。値が 0h なら 1 秒である。8.1.19.4 の節では、この値は CC.SHN の設定から CSTS.SHST が 10b になるまでの時間である。
- **起きる道筋：**
  1. `read_controller` は別の装置を見つけると 561 行で戻る。
  2. `c->rtd3e` を書く 577 行はその先にある。`c->rtd3e` は前の装置の値のままである。
  3. この差分で `nvme_shutdown` は別の装置にも CC.SHN を書く（1043 行）。
  4. `shutdown_done` は前の装置の値で `least` を決める（999 行）。
  5. 別の装置の RTD3E が前の装置より長く、その間 SHST が 00b のままなら、1011 行で待ちを打ち切る。文書が推奨する最短の待ちより短い。
- **影響：** その装置は電源が切れるまでに shutdown を終えないことがある。データは失わない。この OS はその装置に命令を書いていない。1045 行で `all` にも効かない。
- **直し方の案：** 別の装置の判定の前で `c->rtd3e` を読む。577 行を 555 行の前へ動かせば足りる。

### 2. overflow の COMRESET と halt の engine の止め方が、周りの注釈に入っていない

- **重さ：** きれいさ。
- **場所：**
  - `kernel/src/dev/ahci.c:19-21`：COMRESET の条件に overflow が無い。
  - `kernel/src/dev/ahci.c:548-551`：`port_up` の COMRESET の条件に overflow が無い。
  - `kernel/src/dev/ahci.c:788-790`：halt は「the engines stopped」と書く。今は command list の engine だけを止める（817 行）。
  - `kernel/src/dev/ahci.c:294`：「until the engines stop (halt)」も同じである。
- **文書：** AHCI 1.3.1 の 6.1.5。書き込みの overflow の掃除には COMRESET が要る。コードはこのとおりである（309 行）。注釈だけが古い。
- **起きる道筋：** 動作には効かない。次に読む者が、COMRESET の条件と、halt の後の FIS receive の状態を取り違える。
- **直し方の案：** 各行に overflow を足す。halt の注釈は command list の engine と書く。

## 確かめて指摘にしなかったもの

### AHCI

- **`settle` の `IS_OFS` で `need_reset`（309 行）。** 6.1.5 に合う。読み込みの overflow にも COMRESET を打つが、6.1.5 が求める致命のエラーの回復の範囲に入る。`settle` は命令の向きを持たない。COMRESET は `comreset` の中で cache を失ったものとして数える（416 行）。これは安全の側である。
- **halt の中や `p->fatal` の後に来た OFS は `need_reset` にならない（298 行、364 行）。** 書き込みが終わっていない装置は PxTFD が BSY のままである。`port_up` は 576 行で COMRESET へ進む。engine が止まらなければ `port_force_stop` が COMRESET を打つ。したがって COMRESET は抜けない。
- **halt の `list_stop`（817 行）。** 5.3.2.7 と 5.3.2.8 と 5.3.5.3 では、ST が 0 のとき受けた D2H Register FIS は P:RegFisUpdate を経る。FRE が 1 のときだけ P:RegFisPostToMem へ進み、そこで PxTFD を更新する。10.3.2 では FRE が 0 の間、FIS は内部に留まる。したがって FRE を残す差分は 6.2.2.1 の手順に合う。注釈の 3.3.7 は FRE の記述の節で、正しい。
- **`list_stop` の後の DMA。** FIS receive の engine が書くのは port の `p->mem` だけである。`p->mem` は返さない。呼ぶ側の buffer は command list の engine が動かす。halt の約束は保たれる。
- **`port_up` の手順。** 非 queued の道は ST を落とし、CR を待ち、PxSERR と PxIS を消し、PxTFD を見て、ST を立てる。6.2.2.1 の順である。10.3.1 の ST の条件（CR が 0、FRE が 1、BSY と DRQ が 0、DET が 3h）も満たす。`reset` の道は 572 行で `port_stop` の後に memory を渡し直す。10.3.2 の PxFB の条件に合う。
- **`port_force_stop` の後の `port_up`。** FR は止まっている。578 行が FRE を立て直す。PxFB は変えていない。10.3.2 に合う。
- **PxSIG。** ST が 0 の間の D2H Register FIS は P:RegFisUpdate で PxSIG も更新する。これは FRE に関わらず起きるので、差分で変わらない。`port_examine` は `port_changed` の `need_reset` の COMRESET の後に読む。

### NVMe

- **identity の範囲。** 4.5.1 と 4.5.2 では VID は 01:00、SSVID は 03:02、SN は 23:04、MN は 63:24 である。Figure 338 では SUBNQN は 1023:768 である。`identity[64]` と `nqn[256]` と `id + 768` は合う。
- **注釈の 4.7.1.2。** この節は SUBNQN を NVM subsystem の識別に使うとする。1.2.1 より前の装置は VID、SN、MN を組み合わせる。Figure 141 には SSVID も入る。4.7 は NQN を永続とする。差分の照らし方に合う。
- **SUBNQN の無い古い装置。** `identify` が先に `c->ident` を 0 で埋める。両方の回で 0 になり、誤って別の装置と判定しない。
- **別の装置への CC.SHN（1043 行）。** 3.6.1 の手順 1 は enable の時だけの条件で、手順 2 に条件は無い。Figure 41 は電源を落とす前に SHN を書くよう求める。合う。I/O queue はこの時点で無い。1039 行の `live` は `goto down` で偽なので、`delete_queues` を通らない。
- **`c->stranger` の読み。** 559 行で `cut` を release で書く前に立てる。1035 行は `cut` を acquire で読んでから読む。順序は正しい。
- **別の装置へのデータ。** `enable` は Identify の後でしか `ready` を立てない。別の装置の判定で戻れば `ready` は偽のままである。書き込みは届かない。flush は `recover` が偽で `-N_ERR_IO` を返す。成功を偽って返す道は無い。
- **`queue_pass`（275 行、380-381 行）。** PCIe 6.6.2 の FLR の手順の例は、実行する software が他の software と同期することから始まる。`controller_down` の後に両方の lock を通るので、`submit` の doorbell（sq_lock の下）と `reap` の doorbell と `run` の CSTS（cq_lock の下）はここで終わる。`queue_clear` と違って二つの lock を重ねて持たないので、lock の順にも触れない。`stop_lock` は mutex で、spinlock の下ではない。規則に合う。
- **FLR の間に registers へ届く他の道。** `rd32`・`wr32`・`doorbell` の呼出しを全部辿った。`enable` は `controller_stop` が真の後だけである。`nvme_shutdown` は `pci_reset_left` で避ける。`run_polled` は power change の中だけである。抜けは無い。
- **`waited`（450 行、464 行、480 行、481 行）。** 文書が決める読みの時機は無い。最初の待ちの 1 ms で終わる命令は CSTS を読まない。終わらない命令は 1 ms 後に一度読み、それ以降は deadline まで割り込みを待つ。CSTS の致命の表示は、差分の前と同じく一回の読みと deadline の時点で見る。起こしは `seq` の語で失われない。polled の道は毎回読む。

## 差分の外

データを失う問題は無い。`transfer` の書き込みの再試行は、`recover` の `enable` の Identify を通ってから queue が開くので、別の装置へ向かわない。
