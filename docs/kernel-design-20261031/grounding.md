# 網と手元の資料で照らした事実（2026-10-31）

持ち主の依頼「use websearch so we can ground ourselves」。`choice.md` の外の事実を照らした。手元の一次の資料（PPR・仕様書・port したドライバーのコード）があるものはそちらを先に信じ、網は二次の資料として扱う。他の OS の系の報告は、ハードウェアの振舞いの証拠として読み、設計の手本にはしない。

## 1. VRR の画面で、指だけが動いた時

- **一次（手元）**：RTX 4070 の道では、cursor の即時の channel の更新に `RELEASE_ELV` を付けると、窓のフリップなしで一回の走査が始まる（`third_party/nvidia-open/src/nvidia-modeset/src/nvkms-cursor3.c` の `ReleaseElvC5`）。NVKMS 自身は、指が動いてから 33 ms（30 fps）待って走査を始める（`nvkms-vrr.c` の `nvTriggerVrrUnstallMoveCursor`）。理由として NVKMS が書くのは、指に応えてフリップするアプリが、指の起こした走査の途中にフリップしてカクつくこと。
- **網（二次）**：ほかの系でも、VRR の画面で指だけの更新では走査が始まらず、内容の更新が遅い時に指がカクつくと報告されている。系ごとに、下限（30 Hz など）を置く、指で再描画を起こす、起こさない、と扱いが分かれる。出所：Smithay の issue #1610、niri の issue #1214、GNOME Mutter の MR 1154、Sway の PR 7780、KTC の解説。
- **一枚への反映**：指の動きは新しい中身として、位置を書くと同時に走査を始めさせる。NVKMS の 33 ms の待ちは固定の予定なので採らない。

## 2. 指の絵の大きさ

- **一次（手元）**：NVKMS の cursor の能力は `maxSize` 256（`nvkms-cursor3.c` の `nvEvoCursorC5`）。今の `ports/nvidia/display.c` の 32×32 は我々の限り。

## 3. 低い拍の繰り返し（LFC）

- **一次（手元）**：RTX 4070 の道では、表示の装置が最小の拍の間隔（`MIN_REFRESH_INTERVAL`、モニターの申告の最小の拍から 1/fmin）で前の絵をもう一度走査する（`nvkms-evo3.c`）。
- **網（二次）**：LFC は絵の繰り返しで、絵を作るのではない。繰り返すのが GPU 側かモニター側かは、資料によって食い違う。手元のコードを先に信じる。

## 4. VRR の範囲の申告の置き場所

- **網（二次）**：範囲は、EDID の Range Limits の記述子、DisplayID、HDMI Forum の VSDB、各社の VSDB（AMD・NVIDIA）に書かれうる。置き場所どうしが食い違うこともあり、Range Limits の印の付け方を誤るモニターもある。出所：LWN の記事 1059082、LKML の amdgpu の VRR の直しの議論（2026 年 2〜3 月）。
- **一枚への反映**：共通の EDID の読み手は、どの置き場所も読まない（`common/edid.h`）。読む置き場所をすべて足し、食い違いの扱いを決める必要がある。NVIDIA の道では、NVKMS が EDID から最小の拍を計算している（`nvkms-vrr.c` の `GetEdidTimeoutMicroseconds`）。

## 5. VRR のちらつき

- **網（二次、測りあり）**：OLED と VA のパネルは、絵の間隔が大きく揺れると明るさがちらつく。暗い色ほど強い。IPS と TN は良い。出所：RTINGS の VRR flicker の研究、ASUS ROG の解説（各社の主張は割り引く）、BGR。
- **一枚への反映**：パネルの性質として置く。固定の拍を強いる理由にはしない。持ち主の画面のパネルの種類は未確認。

## 6. GPU の横取り

- **網（一次に近い）**：NVIDIA は Pascal から、描画の横取りを画素の粒度で行う。最後の画素の仕事が終わってから 100 µs 以内に文脈を替える（GTX 1080 の whitepaper）。計算の仕事は命令の粒度。横取りは時間の割り当ての切れ目で起きる（Pascal の組み込みの GPU の研究、RTSS 2018）。
- **未確認**：Ada（RTX 4070）での値は見つからなかった。合成の予算の「横取り ≤200 µs」は、第 0 段で測る。

## 7. 速い画面の今

- **網（二次）**：1080p の 600 Hz は売られている（BenQ Zowie XL2586X+、AOC AG246FK6、Acer、MSI）。2026 年には、1080p の 1000 Hz（LG UltraGear 25G590B など）と、Samsung の 1040 Hz が発表・発売されている。
- **一枚への反映**：物差しの一番速い画面を、1080p 1000 Hz（1 コマ 1.0 ms）に上げることを持ち主に提案する（`choice.md` の 7 節）。

## 8. Zen 4 で空間を切り替える値段

- **網（二次）**：Zen 4 の IBPB の値段や、PCID なしの CR3 の書きの値段を測った公開の値は見つからなかった。一般の資料は、CR3 の書きが百 cycle ほどで、PCID が無ければ TLB を全部消すと書く（kernel.org の PTI の文書）。ほかの数字は出所のない見積もり。
- **一枚への反映**：第 0 段で、7600X の実機で測るほかない。A の芯の当否を決める値。

## 出所

- https://github.com/Smithay/smithay/issues/1610
- https://github.com/niri-wm/niri/issues/1214
- https://gitlab.gnome.org/GNOME/mutter/-/merge_requests/1154
- https://github.com/swaywm/sway/pull/7780
- https://us.ktcplay.com/blogs/support-tips/adaptive-sync-cursor-stutter
- https://us.ktcplay.com/blogs/technology-hub/how-does-low-framerate-compensation-extend-adaptive-sync-below-the-minimum-range
- https://www.phoronix.com/news/MTY4ODM
- https://lwn.net/Articles/1059082/
- https://lkml.iu.edu/2602.2/00864.html
- https://www.rtings.com/monitor/learn/research/vrr-flicker
- https://rog.asus.com/articles/gaming-monitors/oled-flicker-what-it-is-what-you-can-do-about-it-and-how-rog-leads-the-fight-against-it/
- https://www.bgr.com/2215855/why-oled-screen-flickering-how-fix-it/
- https://international.download.nvidia.com/geforce-com/international/pdfs/GeForce_GTX_1080_Whitepaper_FINAL.pdf
- https://hipert.unimore.it/people/marko/papers/RTSS18.pdf
- https://tftcentral.co.uk/news/aoc-agon-pro-ag246fk6-esports-gaming-monitor-unveiled-with-600hz-refresh-rate
- https://www.notebookcheck.net/BenQ-Zowie-XL2586X-Plus-New-ultra-fast-600Hz-FHD-gaming-monitor-launches-with-24-5-inch-Fast-TN-panel.940975.0.html
- https://pcgamer.com/hardware/gaming-monitors/lg-reveals-the-worlds-first-native-1-000-hz-1080p-panel-but-youre-gonna-need-a-beast-of-a-rig-to-make-good-use-of-it/
- https://www.tomsguide.com/computing/monitors/samsung-just-broke-the-speed-limit-with-first-1-040hz-gaming-monitor-and-it-comes-with-qhd-support
- https://www.kernel.org/doc/Documentation/x86/pti.txt
