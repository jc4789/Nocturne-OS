# 共通 Fetch の参照元・方針接続：独立第二考察

第一報の実検索失敗と実装範囲を前提に、主担当の反証と別の境界読取から以下を修理・限定した。第一報を成功へ書き換えず、Worker の未成立は残す。

## 反証と製品での扱い

| 反証 | 初版の危険 | 最終の扱い・支持 |
|---|---|---|
| `127.evil.test` | 127 という prefix だけで loopback と誤認し、strict policy の downgrade source を送る | 127/8 は四つの数値 octet を範囲確認。悪性 suffix は不可信。host 反例で抑止を確認 |
| IPv6 loopback | localhost/127 のみでは `[::1]` の信頼判定を落とす | predicate は `[::1]` と `[::2]` を区別。既存 HTTP transport の IPv6 非対応は保全し、実通信対応とは呼ばない |
| `.localhost` | 名前が localhost で終わるだけで native resolver の保証なく信頼を与える | Nocturne の現 resolver/security 境界に合わせ exact localhost のみ。未対応 URL spelling を勝手に正規化・信頼しない |
| Fetch ごとの policy 読取 | resources_dirty の度に doc_sync_tree で全 DOM を走査し、機能成功と引換えに性能回帰を増やす | getter は policy integer の読取だけ。parser は既存 coherent changed publication、connected 挿入は既存 subtree 通知へ統合。Fetch ごとの全同期を撤去 |
| parser meta の後に blocking script | 文書同期を遅らせると最初の Fetch の policy が古い | 既存 parser commit の原順序に局所 meta 処理を入れる。native fixture は response policy と script 前 meta の override を別値で検査。最新版 native 実行は主担当受入待ち |
| sandbox opaque client | 表示 URL/effective URL を無条件で about:client source とすると親の参照元を漏らす | 既存 SB_ORIGIN と effective tuple を native helper で確認。opaque は空 source、fetchOrigin は null。author getter 不実行 |
| srcdoc | srcdoc 自身の URL か無検査の親 URL を使う | frame_parent policy/container chain を辿る。opaque ancestor を source にしない。native helper の五つの scope 反例を支持 |
| Worker blob URL の作者 spelling | `blob:https://…` という文字列を authority として creator origin を上書き | 同一 origin 判定は native securityURL の creator snapshot。location/script URL と native authority を区別 |
| redirect の後に緩い policy | 一度省略した path を元 source から復活できる | hop 後の縮小 source を保持。origin 化・no-referrer からの復活禁止を HTTP helper で検査 |
| author Referer ヘッダー | throw を消すだけで禁止ヘッダー経路を解禁 | author header は依然除去。private source/policy だけが既存 HTTP serializer へ届く |

[Referrer Policy の client/source 決定](https://w3c.github.io/webappsec-referrer-policy/#determine-requests-referrer)、[Secure Contexts の origin 信頼判定](https://w3c.github.io/webappsec-secure-contexts/#is-origin-trustworthy)を境界とした。これは全面規格適合・全 WPT 成功の宣言ではない。

## 寿命・互換・性能の別確認

- caller は指定初期化を使い、新 field を未初期化末尾として読まない。native request の source は pending 所有、wire queue 前に copy、HTTP job 終了・失敗・redirect に応じて cleanup する。Worker requester の借用 bytes/source は callback 内で転送側が copy する。
- wire prefix は 64-byte のまま、extension flag がない旧 packet をその framing で扱う。6-field Worker packet と旧 4-field を区別し、各 field の brand・length・NUL・policy を確認。本文を text 化しない。
- private metadata を console へ出す診断は追加していない。source/report に実参照元の credentials・Cookie・token・私的 query を保存しない。
- C 9 ファイルの構文成功と 167 helper 条件は構造・局所計算の支持であり、実接続・GUI・ページ全体速度の証明ではない。Worker API の追加を有意な並列化の証拠へ昇格しない。

## 原失敗を保全した試験訂正

- 長い source の host 条件は fixture の prefix copy が一 byte 短く、slash を失って hostname を作っていた。strlen による fixture 訂正で期待を維持し、初回の 1 失敗を成功実装の根拠に使わない。
- host Windows の SB_TOP macro と product enum の衝突は fixture 側で undef。native sandbox の意味は不変更。
- Worker private getter 未接続を product native getter へ修理したが、その後の runtime 終了は未解決。原失敗と修正版失敗のログを別名で保全。失敗する POST 条件を除外、skip、true 固定していない。
- 現 Worker fixture は親 endpoint をモデル化し、child と parent の packet を同一 host runtime で処理する。実隔離 Worker と同等の実行・寿命だとは主張しない。終了点の切分け後に native 製品側か fixture側かを確定する。

## 最新実検索とログ解釈の再考察

- native Fetch API 244 / jstest 7 / 既存 pseudo-body 17 は失敗 0 で通ったが、実検索は URL だけが変わり旧 feed のため未達。元 referrer 未対応例外がなくなったことを、検索完了へ昇格しない。
- 最新 AbortError の stack は作者 timer callback → AbortController.abort → abortSignal。native request deadline の文言とは別。誰の signal、どの pending request かはまだ特定していない。
- `pending 20` は全 native pending 件数であり、検索 Fetch 20 件とは読めない。既存 diagnostic に文書一回・profile opt-in の P_FETCH 数値 snapshot を加え、他種 request と区別する。hot path へ全 pending 走査・時計・保存は追加していない。
- `webnet complete` の `decoded-frame-bytes=0` は最後の END frame の body_len。END は frame validation 上 body_len=0 が正しいので、受信本文が空である証拠ではない。該当 stream 完了は実検索 click より前で、別 request の成功を検索成功にしない。
- click 後に complete 行がないことも、submit 自体が記録されない現ログでは未提出の証明にならない。新 snapshot の host-accepted は native queue 受理、headers-published は JS Response publication として区別し、wire id／HTTP 発行／body stream 消費を推定で同一視しない。
- Worker 二回の異なる限定 marker は FormData 作成後、実 RNG と native_request より前で終了する境界を示した。API packet が成功した証拠ではない。marker は撤去済み。LLDB は python310.dll 不足で起動前に終了し、対象 exe を診断実行したと報告しない。

## 結論

共通責務での referrer 実接続は native 支持・全体梱包まで進み、旧実例外は最新実検索で出なくなった。getter 全 DOM 同期、loopback 誤判定、opaque/srcdoc source、blob creator authority の危険は具体反例を受けて修理した。新一回 Abort snapshot まで source 再凍結。Worker 最終支持、実 Reddit 検索内容の成立、最新媒体の他サイト無回帰・性能回帰解消は別の未達条件として保持する。
