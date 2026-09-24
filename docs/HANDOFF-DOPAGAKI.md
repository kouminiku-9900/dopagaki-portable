# 引き継ぎ：dopagaki-portable（2026-09-24）

komi-tube（`fd2c5ee`時点のmain）のファイルをもとに、PSPを縦持ちにして見るYouTubeショート専用アプリとして作ったもの。komi-tubeの履歴は引き継いでいない（新しいリポジトリ）。

## 状態

- `native/dopagaki/`：アプリ本体。`scripts/build_native.sh dopagaki`でビルドし、`scripts/install_dopagaki.sh`で`PSP/GAME/DOPAGAKI`へ入れる。
- **実機の自動試験はPASS**（PSP-3000、PSPLink）：おすすめ3本連続、検索、再生中のいいね、マイリストの再生と削除。最初の映像まで約2.2〜2.9秒、30fps、ヒープ約7〜9MB。
- **操作感（縦持ちでの十字キーの向き、切り替えの速さ）はユーザーの確認待ち。**

## 縦持ちの仕組み

- 持ち方：アナログスティック側（横持ちの左端）が下。
- 描画：`komi_set_portrait(true)`で、プログラムは272×480のキャンバス（`komi_canvas()`）に描く。`komi_canvas_present()`が16×16のタイルごとに90°回転してVRAMへ写す。キャンバスの(x, y)は画面の(479 − y, x)。
- 動画：`komi_playback_frame`が縦向きでは動画をキャンバスへ拡大縮小（`psp_media_scale`の出力上限の高さを272→480にした）し、重ね表示を描いてから回転する。1フレームの描画（拡大縮小・重ね・回転）は平均約30ms。30fpsは保てているが余裕は少ない。重くなったらGEで回転させる案がある。
- `ui.c`は描く面の大きさを`ui_set_surface`で切り替えられるようにした（既定は従来の横向き。komi-appは影響なし）。
- 入力：横持ちの→が上、←が下、↑が左、↓が右。アナログも同じ向きに読み替える（`upright_buttons`）。

## ショートの取得（`native/dopagaki/shorts.c`）

MWEBクライアント（`clientVersion`は`2.20260922.04.00`を固定）でInnerTubeへPOSTする。アカウントなし。

| 用途 | エンドポイント | 中身 |
|---|---|---|
| おすすめの開始 | `reel/reel_item_watch`（`params=CA8%3D`、`inputType=REEL_WATCH_INPUT_TYPE_SEEDLESS`） | 1本と`sequenceContinuation` |
| おすすめの続き | `reel/reel_watch_sequence`（`sequenceParams`） | 1〜12本と次のトークン。動画IDだけでタイトルは無い（再生時に`komi.media.stream.title`から埋める） |
| 検索 | `search`（`params=EgIQCQ%3D%3D`＝種類：ショート） | `shortsLockupViewModel`が約20本。タイトルは`overlayMetadata.primaryText`、チャンネルは`accessibilityText`の右から2つ目 |
| 検索の続き | `search`（`continuation`） | 同上 |

応答の読み取りは`shorts_parse.c`（移植性のあるC）。保存した応答`native/tests/fixtures/shorts-*.json`（visitorDataは消してある）で`scripts/test_native.sh`がMac上でテストする。

## 実機で分かったこと（tilefinchへの変更）

- **縦長360p（360×640、itag 134）はPSPのデコーダーが最初のアクセスユニットで拒否する。** 縦長240p（240×426、itag 133）はwideプログラム（mode 5 / ME boot type 1）で再生できる。
  - `youtube_resolver.c`：画質の比較を「高さ」から「短い辺」に変更（縦長の240×426も240p扱い）。縦長で高さ480を超えるものは候補から外す。これで360p設定のままでも、横長は640×360、ショートは240×426が選ばれる。
  - `media_backend_psp_policy.h`／`media_backend_psp.c`：縦長（幅272以下・高さ480以下）を受け付ける枠を追加（ストライド512、行数は16の倍数に切り上げ）。MEプールの1枠（768×368×4）に収まることを`_Static_assert`で確かめている。
- **プロセス内で最初の縦長ショートが「NO PACKET ACCEPTED」で失敗することがある**（毎回ではない）。同じショートを開き直すと再生できるので、失敗したら1回だけ開き直す（`play_current_retrying`）。原因は未調査（MEの起動直後の状態が関係していそう）。
- Wi-Fiの保存設定1〜4はこの場所では届かず、7番でつながる（ログは番号だけ残す）。

## 次にやるとよいこと

- ユーザーの操作感の確認結果を反映する（向き、ボタン割り当て、重ね表示の量）。
- 切り替えを速くする：次のショートの解決（プレイヤーAPI）を再生中に先に済ませておく。今は1本あたり約2.2〜2.9秒。
- 回転をGEで行い、描画の負荷を下げる。
- 最初の縦長ショートが失敗する原因の調査（`tilefinch-validation.txt`は上限があり古い行が消えるので、失敗直後に止めるデバッグ版で見る）。
