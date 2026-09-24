# dopagaki-portable

PSPを縦持ち（アナログスティック側が下）にして見る、YouTubeショート専用アプリ。komi-tubeのファイルから作った派生（`native/dopagaki/`）。

- 現在の状況と次の作業：`docs/HANDOFF-DOPAGAKI.md`（最初に読む）

以下はkomi-tube由来の部分の説明。

PSP単体で動くYouTubeクライアント。tilefinch（`vendor/tilefinch/`、ブラウザ）ベースの現行版から、ブラウザを使わないネイティブクライアントへ作り替えている途中。

- komi-tubeネイティブ版の経緯：`docs/HANDOFF-NATIVE.md`
- tilefinch版の経緯とビルド・実機手順：`docs/HANDOFF.md`
- 実機テストの手順：`docs/FIELD-TEST.md`

ユーザーの手間とPSPの持ち出し回数を最小にすること。実機で動かすEBOOTは起動するだけで試験を最後まで実行し、ログをメモリースティックに残す作りにする。
コミットメッセージ・コメントは既存に合わせる（ドキュメントは日本語）。
