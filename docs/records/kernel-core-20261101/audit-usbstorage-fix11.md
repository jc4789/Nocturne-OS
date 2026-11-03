# USB hub の driver の差分の監査（十一回目）

## 依頼文

Nocturne OS（独自の OS、カーネルは C、対象は実機）の USB hub の driver の差分を、公式の文書と敵対的に照らしてください。返事と報告は日本語です。

作業場所：`D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64`（読むだけ。コードは変えない）。

材料：
- 差分：`git diff 592078e0 HEAD -- kernel/src/dev/usbhub.c`。周りのコードは今のファイルで読む。
- 文書の text（節番号を自分で確かめる）：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\` の usb20.txt、usb32.txt。原本は `D:\Programes\OS DEV INFO`。
- カーネルの規則：`.claude/rules/kernel.md`。

範囲：差分の行が生んだ欠陥か、差分の行そのものと文書の差だけを挙げてください。差分の外は挙げないでください。推測で疑わず、一件ごとにコードの場所と文書の節を確かめてから書いてください。`docs/` の下の他の監査の報告は読まないでください。Linux その他の OS のソースは参照しないでください。

報告：全文を `docs/records/kernel-core-20261101/audit-usbstorage-fix11.md` に書いてください（先頭にこの依頼文）。各指摘の形：重さ（当たれば壊れる／きれいさ）、場所（ファイル:行）、文書の節と日本語の要旨（原文を長く写さない）、起きる道筋、直し方の案。指摘が無ければ無いと書いてください。コミットはしないでください。最後は SubagentHandback で、件数と重さごとの内訳と報告のパスを返してください。

## 結論

指摘は 1 件。

- 当たれば壊れる：0 件
- きれいさ：1 件

証拠の範囲はコードと文書の静的な照合だけ。ビルドと実行はしていない。

## 指摘

### 1. 一度の読み損ないで、動いている hub を毎秒読み続ける

- **重さ：** きれいさ。機能は壊れない。バスと worker の仕事が hub の寿命の間ずっと残る。
- **場所：** `kernel/src/dev/usbhub.c:57-59`（`asking` の注釈）、`:86-90`（`hub_ask`）、`:129`（`port_changes` の呼出し）、`:282`（`hub_status_read` の呼出し）。消す側の不足は `:492-499`（`usbhub_work`）。
- **文書：**
  - USB 2.0 11.12.2（USB 3.2 10.13.2 も同じ）：変化の検出には Status Change endpoint の interrupt pipe を使う。
  - USB 2.0 11.12.2（USB 3.2 10.13.2 も同じ）：change bit が立っている間、hub は報告を続ける。
  - USB 2.0 11.12.3、Figure 11-21（USB 3.2 10.13.3、Figure 10-23 も同じ）：status の読みと確認は報告を受けて行い、終われば interrupt pipe に戻る。
  - 文書は余分な GET_STATUS を禁じていない。文書との差は、報告が動く hub の変化を、報告でなく毎秒の読みで見続ける点にある。
- **起きる道筋：**
  1. `asking` を立てる場所は `hub_ask` だけで、下ろす場所はファイルのどこにも無い。
  2. 差分は `port_changes` の読み損ないにも `hub_ask` を足した。`hub_status_read` の読み損ないも、確認の済んだ変化があれば `hub_ask` を呼ぶ（接続の時の `usbhub_attach` を含む）。
  3. `h->reporting` が真の hub で、確認の後の GET_STATUS が一度だけ失敗すると、`asking` が立つ。
  4. 次の読みが通って `held`・`hub_held` が空になっても、`asking` は立ったまま残る。
  5. それ以後、`usbhub_work` は毎秒 `hub_status` を呼び、全部の port に印を付ける（`:493`、`:498-499`）。
  6. 1 秒ごとに hub の GET_STATUS が 1 回、port の GET_STATUS が port の数だけ、抜かれるまで走る。
  7. 差分が書いた `:57-58` の注釈は「確認した変化がまた答えるのを待つ」と書く。待つ相手が答えた後も読み続けるので、注釈と動きが違う。
- **直し方の案：** `usbhub_work` の port の輪の後で、次の全部が真なら `asking` を下ろす。
  - この回が `ask && answered` である。
  - `h->reporting` が真である。
  - `h->hub_held` が偽である。
  - 全部の port の `h->held[p]` が 0 である。

  報告の無い hub は `h->reporting` が偽なので、読み続ける。`hub_ask` は `ask_gap` を 1 に戻すので、次に立てるときの間隔も正しい。残った `ask_at` は 1 秒より先を指さない。

## 調べて指摘にしなかったこと

差分の行ごとに、次を確かめた。

| 調べたこと | 結果 |
|---|---|
| `port_changes` の `held` の受け渡し | 読みが通れば全部を `*change` へ渡して 0 に戻す。確認の済んだ bit をもう一度確認しない。USB 2.0 Figure 11-21 の「変化を貯めて確認し、無くなったら処理する」に合う。 |
| `held` が立つ間に子ができる道 | 無い。子を作るのは `hub_port` で、`port_changes` が真を返した後だけ。 |
| `:415` と `:445` の読み損ない | `*change` が 0 でなければ `hub_ask` が再度の読みを保証する。`*change` が 0 なら change bit は hub に残り、hub が報告を続ける（USB 2.0 11.12.2、USB 3.2 10.13.2）。 |
| `:445` の後で `change` を捨てること | `:445` の後に `change` を読む行は無い。 |
| `:417` の新しい条件 | `t->at` が 0 のときの再試行を消した分は `hub_ask` が受ける。`usb_port_trying` は stuck を除くので、消した `usb_port_failed(t, true)` は `hc_busy` に効いていなかった。起動の待ちは変わらない。 |
| `hub_ask` の早い戻り | `ask_gap` と `ask_at` を保つだけで、`usbhub_reports_lost` の `h->reporting = false` は呼ぶ前に済んでいる。 |
| `hub_ask` の呼ばれる thread | `port_changes`・`hub_status_read`・`hc_recover` は全部 controller の worker で走る。`asking` と `held` に錠は要らない。 |
| 接続のときの `hub_status_read`（`:255`） | `usbhub_attach` は hub ごとに一度だけ（`xhci.c` の `hub_taken`）。読みが通れば確認の済んだ変化は直後の `power_ports` が受ける。`C_HUB_LOCAL_POWER` を消してから PORT_POWER を送るので、USB 2.0 11.11 と USB 3.2 10.12 の「両方が 1 なら電源は入らない」に当たらない。 |
| 接続で読み損なった場合 | `hub_held` が残り、後の `hub_status` がもう一度 `power_ports` を呼ぶ。出る行は「power good again」で実際とずれ、`power_ms` を一回余分に待つが、害は無い。読み損なった時に `h->off` が偽のまま電源を入れるのは、差分の外の `:256` の動き。 |
| `hub_status` の分割 | `hub_change` を消費して最初の読みが失敗しても、確認していないので change bit は残り、再び報告が来る。確認の後の失敗は `hub_held` と `hub_ask` が受ける。 |
| 記憶の確保と解放 | `held` は `prepare` の失敗の道と `usbhub_detach` の両方で解放する。大きさは `ports + 1` で、番号 1 から使う。 |
| 注釈の引用 | USB 2.0 Figure 11-21 と USB 3.2 Figure 10-23 は、どちらも「Port Status Handling Method」で、hub の status の読みも含む。 |
