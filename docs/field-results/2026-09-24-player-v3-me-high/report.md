# komi-player：H.264とAACを拡張領域で動かす試験（2026-09-24）

`komi-player.cfg`に`me_high=1`を置き、起動直後に0x0A000000の少し先までを重し（約22MB）で埋めてから、Media Engine用の領域を確保させた。

- `komi-player-2videos-memalign.txt`：`memalign`を記録した短い実行。Media Engine用の領域（5,538,132バイト、4MB境界）は**0x0A400000（拡張領域）**に置かれた。デコーダーのバッファはすべてこの領域から取られる（ほかに4KB以上の`memalign`は無い）。
- `komi-player-10videos.txt`：同じ条件で10本×15秒。**10本すべて合格**、落ちたフレーム0、音声の欠落0、最初の映像まで2.2〜4.1秒。

結論：PSP-3000（6.61 PRO-C）では、**Media EngineはH.264（sceMpeg / mpeg_vsh）でも拡張領域のバッファを読み書きできる**。memprobe v2のAACの結果と合わせ、tilefinchの`media_backend_psp_pool.h`冒頭の前提（MEは0x0A000000以上を読めない）はこの機種では当てはまらない。
ネイティブ版では、Media Engine用の領域を低位に固定する必要はない（固定しておいても害はないが、低位の2MB強を占有しない設計にできる）。
