# 媒体担当 第二引継ぎ

- 製品実装は既存 `docs/media-timing-20261012.md` に記録済み。最新協調MSEのslot/revision更新、空probe背圧、有限pumpの実device snapshot集約は構文検査済みであり、実サイト受入成功とは区別する。
- 主担当の追加修理を独立ソースreviewした。`pump_impl`はrevision同期とseek/preparation処理後、BUSY/AGAIN/input-waitで戻る前に `present_ahead` を呼ぶ。既存のgeneration/revision、実currentと作者PTS、ownedpixel長の検査を保持し、既にdueな画像を新decoder応答なしで解放する。エラー時close/clear/dirtyも整合し、未来画像の早送りはない。追加修理は不要と判断した。
- 根拠は実flowログの音声5414ms・映像5200ms・ahead2@5267msと現在の `user/libc/web/avmedia.c`。この媒体の表示停止は不合格だった。最新修理後の実YouTube/Shaka再生・停止・シーク・再開、聴取品質は未受入。
- この引継ぎではコード編集、VM起動、試験、buildは行っていない。主担当が最新版配布媒体を梱包し実受入へ進む。成功済fixtureは反復しない。
