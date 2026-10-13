# ロード中入力の別反例考察・第二報

1. 非passive listenerがあるwheelまで先にscrollする案を棄却。原DOM/Pointer Eventsの
   取消保証が必要で、今回も通常cancelable dispatchが先、許可時だけ既定scrollになる。
2. 解除mirrorを先に更新する隙間は反例になるため、entry.removedを先に設定した。
   重複登録はcountを増やさず、once/AbortSignal/inline解除/GCで同じ寿命を維持する。
3. DOM wrapper内のmodule待ちは深度が残る。native_waitだけで許可せず、明示snapshot
   準備後の整合判定成功をnative_wait_snapshotへ保存。C checkpoint/active layoutは
   prepared_waitでも常に拒否し、取消後の不整合snapshotをresize/F12でpaintしない。
4. queueの適用済wheelをcheckpoint毎に再走査する案を棄却。head/scan索引により
   新入力だけを扱い、整合が失われた時は次の安全境界まで保留する。
5. 適用済packetの原targetは子documentのidentity/streamを保持して取消・遷移後に
   新documentへ通知しない。ただしdirty C中の未適用wheelは原hitを安全に捕捉できず、
   top document epochだけで後の通常handlerが再hit-testする。この子iframe原target保証は
   未完成。通常DOM revisionで全入力を破棄する偽修理は入れていない。
6. 半構築中の長いC処理から安全なpaintはできない。この修理単独で全ロードの
   応答改善やユーザー回帰解消を宣言せず、主担当/font担当の長task修理と実条件で確認する。

製品sourceと生成bindingは最後のprepared_wait引数修理後に凍結。
構文/host確認とnative fixture/実サイト/性能比較は別の証拠で、後三者は未受入。
