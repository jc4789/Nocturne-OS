# 媒体 resize 追加修理 第一報

- 親が統合媒体を実行中に、occupied の旧 object を含まない `video_full` が連続寸法増加で二枠 transfer window を超えるという境界を発見。親へ通知して `user/apps/browsermseworker.c` の current + retired 全 occupied 合計によるフロー制御へ修理した。kernel / ABI / scaler / PTS / paint の source は変更していない。
- 製品の `video_full` / `video_storage_busy` を直接使用する `tests/media_shared_window_hosttest.c` の新十三条件を一回成功確認。native worker clang syntax と owned diff check exit0。初期 host compile の native strlcpy 宣言不足と外部 entry 残留による link 失敗は試験の linkage を訂正、製品 predicate と期待値は差替えず十三件0失敗。
- 過去の first/second 報告は保存。controller は既存 round2 後に同じ round2 を追加できないため、この追加修理の証拠 binding だけ新しい first/second pair を作る。
- 次の第二考察は EOF / sequence / PCM horizon の製品 callsite を静的に再考する。親の第一媒体 guest が kernel/sharedpages を確かめるなら同じ ABI 検査は反復しない。最終 worker の実動画/ABR/seek は親担当で未確認。
