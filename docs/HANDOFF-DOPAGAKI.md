# 引き継ぎ：dopagaki-portable（2026-09-24）

komi-tube（`fd2c5ee`時点のmain）のファイルをもとに、PSPを縦持ちにして見るYouTubeショート専用アプリとして作ったもの。komi-tubeの履歴は引き継いでいない（新しいリポジトリ）。

## 状態

- `native/dopagaki/`：アプリ本体。`scripts/build_native.sh dopagaki`でビルドし、`scripts/install_dopagaki.sh`で`PSP/GAME/DOPAGAKI`へ入れる。
- **実機の自動試験はPASS**（PSP-3000、PSPLink）：おすすめ3本連続、検索、再生中のいいね、マイリストの再生と削除。最初の映像まで約2.2〜2.9秒、30fps、ヒープ約7〜9MB。
- **操作感（縦持ちでの十字キーの向き、切り替えの速さ）はユーザーの確認待ち。**

## 縦持ちの仕組み

- 持ち方：アナログスティック側（横持ちの左端）が下。
- 描画：`komi_set_orientation(KOMI_PORTRAIT_DIRECT)`。縦のキャンバス座標(x, y)は画面の(479 − y, x)＝バックバッファ`[x*512 + 479 - y]`。`ui.c`は`ui_set_surface`のステップでこの変換を通して直接描く（既定は従来の横向き。komi-appは影響なし）。動画はGEで描く（下の「音切れ」の節）。
- 入力：横持ちの→が上、←が下、↑が左、↓が右。アナログも同じ向きに読み替える（`upright_buttons`）。

## 再生中の「バリバリ」（音切れ）の調査と対策（2026-09-24）

- **症状**：縦向きで再生すると音が細かく途切れる。
- **測り方**：tilefinchの音声スレッドに、`sceAudioOutputBlocking`を呼ぶのが前の呼び出しの戻りから1ブロック（約23ms）以上遅れた回数と長さを数える仕組みを足した（`media_psp_backend_audio_gap_counters`）。1本ごとに`play end ... gaps=N gap-ms=.. gap-max=..`としてログに出す。komi-playerも同じ値を出す。
- **原因**：コーデックの作業スレッド（優先度0x19）は、メインスレッドの`psp_media_feed_before_blocking`で1件受け取るたびに動き、次の受け渡しまで待つ。縦向きの描画（拡大縮小＋90°回転）はCPUで1フレーム15〜18msかかり、vblankに間に合わずにループが30〜40回/秒に落ちる。そのため受け渡しが足りず、音声のデコードが間に合わなかった。

| 方式（各15秒×4本） | 音切れ（1本あたり） | 無音の合計 |
|---|---|---|
| 横向き（komi-player、映像のみ） | 0回 | 0ms |
| 縦・キャンバス＋回転（最初の版） | 約125回 | 約3.3秒 |
| 縦・CPUで回転しながら直接描画 | 約120〜160回 | 約2.3〜3.3秒 |
| 上＋描画中に2msごとに受け渡し（feed tick） | 9〜36回 | 0.1〜0.5秒 |
| GE（EDRAMから読む）＋feed tick | 50〜80回 | 1.3〜2.1秒（GEが主記憶のテクスチャを読むと27ms/フレーム） |
| **GE（EDRAMへDMAで写してから）＋feed tick（採用）** | **0回** | **0ms** |

- **採用した方式**（`komi_runtime.c`の`KOMI_PORTRAIT_DIRECT`）：デコード済みの面をDMA（`sceDmacMemcpy`）でEDRAMの`0x0CC000`（表示用の16bitバッファ3枚の後ろ）へ写し、GEが回転・拡大縮小（バイリニア）・565への変換を行う（約3.8ms＋4.2ms。どちらもメインスレッドは待ち状態なので作業スレッドが動ける）。帯の塗りと重ね表示はCPUが回転した座標で描く（`ui_set_surface`のステップ）。描画中は`komi_feed_tick`で2msごとに受け渡す。
- GEが使えないときはCPUで16行ずつ拡大縮小して回転する方式に戻る。`dopagaki.cfg`の`draw=cpu`／`draw=canvas`で比較用に切り替えられる（`feed_ticks=0`、`autotest_shots=0`も）。
- ソース列を縦になめて一度に回転する方法は5倍遅かった（2KiBおきの行がキャッシュの同じセットに集まる）。

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
- ネットワークが不安定なとき、ショート一覧の取得（`fetch_request_cancelable`、15秒の時間制限）が120秒以上戻らず、見張りで終了したことがある（2026-09-24、1回）。時間制限が効いていない可能性がある。
- 最初の縦長ショートが失敗する原因の調査（`tilefinch-validation.txt`は上限があり古い行が消えるので、失敗直後に止めるデバッグ版で見る）。
