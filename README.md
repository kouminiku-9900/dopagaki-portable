# dopagaki-portable — PSPを縦に持って見るYouTubeショート専用アプリ

![dopagaki-portable](assets/ICON0-dopagaki.png)

PSPを**縦持ち**にして、YouTubeのショート動画を十字キーで次々に見るためのアプリです。PSP自身がWi-FiでYouTubeへ接続し、再生にPCや中継サーバーは使いません。

[komi-tube](https://github.com/kouminiku-9900/komi-tube)（PSP単体で動くYouTubeクライアント）のネイティブ版をもとに作った派生プロジェクトです。通信・再生の部品（[Tilefinch](https://github.com/stjanovitz/tilefinch)）はkomi-tubeと同じものを使っています。

## 持ち方

**アナログスティックのある側（横持ちのときの画面左側）を下にして**縦に持ちます。十字キーとアナログスティックが下、○×△□が上に来ます。

## 操作

| ボタン（縦持ちで見た向き） | 見ている間 |
|---|---|
| 下（横持ちの←）／アナログを下へ | 次のショート |
| 上（横持ちの→）／アナログを上へ | 前のショート |
| 左・右 | 5秒戻る・進む |
| 決定ボタン（本体設定の○または×） | 一時停止・再開 |
| △ | いいね（マイリストに追加・解除） |
| START | ショートを検索 |
| SELECT／戻るボタン | メニュー |
| HOME | PSPの終了画面 |

- 起動するとWi-Fiにつながり、すぐに**おすすめのショート**（YouTubeのショートタブと同じもの）が流れます。最後まで見ると次へ進み、残りが少なくなると自動で続きを読み込みます。
- **検索**：本体のキーボードで入力します（キーボードは横向きに表示されるので、そのときだけ横持ちにしてください）。ひらがなで入力すると漢字の変換候補が出ます。検索結果のショートだけが順に流れます。
- **マイリスト**：△でいいねしたショートは、PSP本体の中（`dopagaki-mylist.txt`）だけに保存されます。メニューの「マイリスト」で順に見られます。
- 再生中は画面が暗くならず、自動スリープもしません。
- 決定・戻るは本体設定（○決定／×決定）に合わせます。

## 入れ方

1. `dopagaki-portable.zip` を展開し、中の `PSP/GAME/DOPAGAKI` をメモリースティックの同じ場所へコピーします。
2. PSPの「設定 → ネットワーク設定 → インフラストラクチャーモード」でWi-Fi接続を保存し、接続テストが通ることを確認します。
3. 「ゲーム → メモリースティック」から **dopagaki-portable** を起動します。前回使ったアクセスポイントへ自動で接続します。

## 対象と制約

- CFWなどでHomebrewを起動できる **PSP-2000以降（64MB）** が対象です。実機確認はPSP-3000（6.61 PRO-C）。PSP-1000とPSP StreetはWi-Fiやメモリの条件を満たしません。
- PSPは2.4GHz・802.11bのWi-FiとWPA2-PSK(AES)またはWPA(TKIP)までに対応しています。
- ショートは**240p（240×426）**で再生します。画面（縦272×480）とほぼ同じ大きさです。PSPの動画デコーダーは縦長の360p（360×640）を受け付けないことを実機で確認したため、縦長の動画はこの画質を選びます。
- ログイン、年齢制限、会員限定のショートは再生できません。再生できないショートは飛ばして次へ進みます。
- おすすめ・検索はYouTubeの非公式API（InnerTube）を使います。YouTube側の変更で動かなくなることがあります。

### 通信先について

PSPから次のサービスへ直接接続します。APIキーやアカウント情報は使わず、アプリにも含めていません。

- YouTube（`m.youtube.com` のショート・検索・動画情報のAPI、動画本体の配信サーバー）
- 漢字変換のみ：Googleの日本語変換（`www.google.com/transliterate`）と検索候補（`suggestqueries.google.com`）。ひらがなで入力した検索語がGoogleへ送られます。

## 開発

セットアップはkomi-tubeと同じく **Apple SiliconのmacOS** 用です（Xcode Command Line Tools、Git、curl、Python 3.12以上）。

```sh
./scripts/bootstrap.sh                 # SDKなどを tools/ に用意（初回のみ）
./scripts/build_native.sh dopagaki     # PSP用にビルド
./scripts/test_native.sh               # Mac上のテスト（ショートの読み取りを含む）
./scripts/install_dopagaki.sh          # PSPLink経由、またはマウントしたスティックへ入れる
```

- `native/dopagaki/`：アプリ本体。`main.c`（画面・操作・再生の流れ）、`shorts.c`（YouTubeへの問い合わせ）、`shorts_parse.c`（応答の読み取り。Macでテスト）。
- `native/common/komi_runtime.c`：Wi-Fi、通信、再生、画面の共通部分。縦持ち用に、272×480のキャンバスへ描いて90°回転して表示する処理を追加しています。
- `native/app/`：komi-tube（新）のクライアント。漢字変換・マイリスト・文字描画はdopagakiからも使います。
- 実機での自動試験：ビルド先（`vendor/tilefinch/build-preset-psp/dopagaki/`）に `dopagaki.cfg`（`autotest=1`）を置き、PSPLinkで `dopagaki.prx` を起動すると、おすすめ3本・検索・いいね・マイリストを自動で試して `dopagaki.txt` と縦向きのスクリーンショットを残します。詳しくは [docs/HANDOFF-DOPAGAKI.md](docs/HANDOFF-DOPAGAKI.md)。

komi-tube由来のブラウザ版・ネイティブ版のビルド手順や経緯は [docs/HANDOFF.md](docs/HANDOFF.md)、[docs/HANDOFF-NATIVE.md](docs/HANDOFF-NATIVE.md) にそのまま残しています。

## 出典・ライセンス

このリポジトリ独自のコード・スクリプト・アイコンはMITライセンスです。komi-tube（MIT）をもとにしています。PSP本体側の通信・再生のコード（`vendor/tilefinch/`）は [Tilefinch](https://github.com/stjanovitz/tilefinch)（MIT、© Steven Janovitz）を取り込んで変更したものです。上流ソースとその依存ライブラリはそれぞれのライセンスに従い、配布ZIPの `NOTICES` に収録しています。YouTube、Sonyとは無関係の非公式プロジェクトです。
