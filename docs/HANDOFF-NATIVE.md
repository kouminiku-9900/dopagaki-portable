# 引き継ぎ：ネイティブクライアントへの作り替え（2026-09-24）

クラウドのセッションからローカル（Mac上のClaude Code）へ引き継ぐためのメモです。tilefinch版の経緯は [HANDOFF.md](HANDOFF.md)、実機テストの手順は [FIELD-TEST.md](FIELD-TEST.md) を参照してください。

## 最初にやること

```sh
git fetch origin
git checkout claude/wonderful-cori-e21ml3   # 作業ブランチ（mainには未マージ）
./scripts/bootstrap.sh                       # tools/ が無ければ
./scripts/build_memprobe.sh                  # 調査EBOOTがビルドできることの確認
```

- ブランチは3コミットだけmainより先に進んでいる：調査EBOOT、その修正（v2）、実機結果の記録とMEMSIZE=1への変更。PRはまだ作っていない。
- `./scripts/build.sh`（tilefinch版のフルビルド）はローカルで通ることを確認済み（下の「未確認・注意点」参照）。

## ゴールと方針（ユーザーと合意済み）

tilefinch（PSP向けブラウザ）にYouTubeを表示させる今の構成から、ブラウザを使わないネイティブクライアントへ段階的に作り替える。対象はPSP-2000以降（64MB）。最初の山場は「ブラウザなしで動画を連続再生できるEBOOT」。

ユーザーの強い要望：**ユーザーの手間とPSPの持ち出し回数を最小にする**。実機で動かすEBOOTは、起動するだけで試験を最後まで実行し、ログをメモリースティックに残す作りにする。ボタン操作や画面の書き写しは求めない。

| 段階 | 内容 | 状態 | 実機 |
|---|---|---|---|
| 0前半 | メモリ配置の実測 | **完了** | 済 |
| 0後半 | 部品を`derived/`へ切り出し、`net/http.h`を定義 | 未着手 | 不要 |
| 1 | ブラウザなしの再生EBOOT（OSKで動画ID → `yt/resolver` → `media/Player`）、`profiles.cfg`の外部化 | **ほぼ完了**：10本連続再生（360p）とH.264の拡張領域試験は実機で合格（下記）。OSK入口、`profiles.cfg`は未 | 済（PSPLink） |
| 2 | HTTPの自前化（mbedTLS＋curlを直接、Range、gzip、リダイレクト、キャンセル） | 未着手 | 1回目か3回目に同乗 |
| 3 | ネイティブ画面（検索→結果（サムネイル）→再生）＋マイリスト | **動作版あり**（`komi-app`、下記）。サムネイル未 | 自動試験は済。操作感はユーザー確認待ち |
| 4 | 関連動画、設定、字幕、履歴、スリープ復帰、PSP goの`ef0:` | 未着手 | 3回目に同乗できるかも |
| 5 | `vendor/tilefinch`を削除し、`derived/`とNOTICEだけ残す | 未着手 | 不要 |

### 段階0後半の具体的な作業

- tilefinchから次を`derived/`へ移す。ファイル先頭に元コミット`5a77720`（stjanovitz/tilefinch）とMITの表記を残す。
  - `media_mp4`、`media_backend_psp`、`psp_media_present_ge`、`psp_network`、`youtube_subtitles`
  - mbedTLSとcurlのビルド設定（`vendor/tilefinch/cmake/PspOwnedTransport.cmake`と`vendor/tilefinch/patches/`のmbedtls・curl分）
- `net/http.h`（抽象インターフェース）を定義する。最初の実装は既存の`fetch`を包むアダプタでよい。
- 既存のブラウザ版EBOOT（`./scripts/build.sh`）も並行してビルドできる状態を保つ。

### 段階1で必ず入れること（実機1回で済ませるため）

- 起動すると、テスト用の動画IDリスト（`data/`に置くテキスト）を自動で順に再生し、1本ごとに確保量、失敗、所要時間をログに残す。OSK入力は手動確認用の入口として残す。
- ~~H.264が拡張領域のバッファを読めるか~~ → **読める**（2026-09-24、`komi-player`の`me_high=1`で確認）。
- `yt/resolver`は既存の`vendor/tilefinch/src/youtube_resolver.c`（クライアント設定、フォーマット選択、STSキャッシュ）から移植する。`yt/`とdemuxは、Mac上の実通信テストとfixtureテストの両方を用意する。
- 合格ライン：実機で10本連続再生して、確保失敗も断片化も起きないこと。

## 段階1の現状（2026-09-24）

順番を変えた：部品の`derived/`への切り出し（段階0後半）より先に、**tilefinchをライブラリとしてそのままリンクし、ブラウザを通さず再生部分だけを動かす**試験プログラムを作った。再生部分（`psp_media_session`ほか約1.2万行）はブラウザ本体と深く結びついていて、切り出す前に「ブラウザなしで動く」ことを実機で確かめる方が安全なため。

- `native/player/main.c`：`komi-player`。起動すると`komi-videos.txt`の動画を順に開き、30秒（または最後まで）再生して閉じ、1本ごとの結果を`komi-player.txt`に書く。ボタン操作なし。120秒進まなければ見張りスレッドが終了させる。描画はtilefinchのソフトウェア拡大（Sharp相当）を16bit画面へ。
- `native/player/player.cmake`：`vendor/tilefinch/CMakeLists.txt`末尾の`KOMI_EXTRA_CMAKE`フックから読み込まれ、ブラウザと同じ`tilefinch_core`と通信ライブラリにリンクする。PRX（PSPLink用）とEBOOTの両方を出す。
- `scripts/build_player.sh`（ビルドのみ）、`scripts/run_player.sh`（ビルド→PSPLinkで起動→終了を待ってログを`field-logs/player-<日時>/`へ）。前回の実行がまだ動いていれば終わるまで待つ。
- **結果：10本すべて合格**（`docs/field-results/2026-09-24-player-v1/report.md`）。最初の映像まで2.2〜5.2秒、読み込み待ち0、ヒープのピーク8.8MB、4本目以降の増加は1本あたり数KB。

tilefinchをブラウザなしで動かすときに要ったこと（次に同じことをする人向け）：

- **通信の許可**：`fetch_background_transport_set_admission(true)`を呼ばないと、すべての通信が「transport admission timed out (network-rejoin)」で拒否される。ブラウザはネットワークの状態管理が準備完了を報告したときにこれを開ける。
- **Wi-Fi**：ダイアログなしの`psp_network_begin`＋`psp_network_pump`で接続できる。ただし保存設定のうち今いる場所で届くもの（このPSPでは7番）を選ぶ必要がある。`komi-player`は保存設定を全部列挙し、前回成功した番号（`komi-wifi.txt`）から順に試す。`config/boot.cfg`の`network_profile=1`は届かない設定だった。
- **ログ**：リリース設定では`psp_log_*`がマクロで空になり、`src/psp_log.c`も空関数になる。`TILEFINCH_PSP_VALIDATION_LOG`を全体に付けると`tilefinch_core`と構造体の形がずれる（`media_backend.h`、`psp_display.h`、`psp_ui.h`に条件付きのメンバーがある）。そこで`TILEFINCH_PSP_LOG_IMPLEMENTATION=1`を付け、`psp_log.c`だけを有効化した版（`native/player/psp_log_enabled.c`）をコンパイルしている。`tilefinch_core`の`printf`は`--wrap=printf`で同じログへ流す。
- **メモリの測り方**：`PSP_HEAP_SIZE_KB(-1)`で区画全体をnewlibが最初に取るため、`sceKernelTotalFreeMemSize`は常に約2.3MBしか示さない。実際の使用量は`mallinfo().uordblks`で見る。
- **360p**：`media_psp_backend_set_wide_program("")`（空＝実機で確認済みのワイド再生）を呼ばないと、バックエンドは240pの互換モードのままになる。ブラウザは`boot.cfg`の`experimental_wide_video`（既定は空）から呼んでいる。呼ぶようにしてから、10本すべて640x360/480x360（itag 134＋140）で合格（`docs/field-results/2026-09-24-player-v2-360p/`）。
- **H.264も拡張領域で動く**（`docs/field-results/2026-09-24-player-v3-me-high/report.md`）。Media Engine用の領域を0x0A400000に置いても、10本すべて合格した。
- `komi-player.cfg`（任意）：`me_high=1`（拡張領域試験）、`play_seconds=N`、`max_videos=N`。

段階1で残っていること：

1. OSKで動画IDを入れる手動確認用の入口、`profiles.cfg`の外部化。
2. その後に段階0後半（`derived/`への切り出しと`net/http.h`）。`komi-player`がリンクしている範囲が、切り出すべき部品の一覧になる。
3. `media_backend_psp_pool.h`の前提（MEは拡張領域を読めない）が誤りと分かったので、ネイティブ版ではME用領域を低位に固定しない設計を検討する。

## 段階3の現状：komi-app（2026-09-24）

ユーザーの追加要望（漢字検索、端末内の「いいね」＝マイリスト）を入れたネイティブ版クライアント。PSP本体に`PSP/GAME/KOMI_NATIVE`（XMBでは「komi-tube（新）」）として入れてある。現行のkomi-tube（ブラウザ版）はそのまま残している。

- 画面：ホーム（検索する／マイリスト）→ 検索（本体のOSK。日本語設定なら漢字変換を含む全モード）→ 結果12件（タイトル、チャンネル、再生数・投稿時期、長さ、マイリスト済みは♥）→ 再生（上にタイトル、下に位置・操作。3秒で消える）。
- 操作：○決定/再生/一時停止、△いいね（マイリストに追加・解除）、□再検索、×戻る、←→10秒送り、ホームでSTART終了。
- マイリスト：`komi-mylist.txt`（EBOOTの隣、1行1本、新しい順、最大200本）。一時ファイルに書いてから置き換える。
- 描画：16bit画面にソフトウェアで描く（`native/app/ui.c`）。ラテン文字はTilefinchSans、日本語は`tilefinch_core`に組み込まれたUnifontの16ドットビットマップ。**日本語は16ドット以外にすると潰れて読めない**。△・▶・Ⅱはフォントに無いので、ボタン記号と再生/停止は私用領域の文字（`UI_ICON_*`）を自前で描く。
- 検索：`youtube_lite`の検索（JSON API）が作るHTMLのカードを`native/app/yt_results.c`で読み戻す。まれに0件の応答が来るので1回だけ自動で再検索し、0件だった応答は`last-empty-search.html`に残す（fixtureにして原因を調べる）。
- Wi-Fi：XMBから起動したときは現行版と同じ本体の接続画面（前回のAPに自動接続）。PSPLink（`host0:`）から起動したときは保存設定を画面なしで順に試す。
- 共通部分（ログ、見張り、ME領域、画面、Wi-Fi、セッション、再生と描画）は`native/common/komi_runtime.c`にまとめ、`komi-player`もこれを使う。
- 自動試験：`komi-app.cfg`に`autotest=1`、`autotest_query=猫`を置いて`scripts/run_native.sh komi-app`。検索→いいね→再生→マイリスト→再生→削除を行い、要所の画面をBMPで保存する。**実機でPASS**（検索約2秒、最初の映像まで2〜4秒、ヒープ約7〜9MB）。
- Macのテスト：`scripts/test_native.sh`（検索カードの読み取りfixture、マイリストの往復）。`--live 猫`で実際の検索も確認できる。
- 入れ方：`scripts/install_native.sh`（PSPLinkがつながっていればそれで、無ければマウントされたスティックへ。マイリストとWi-Fiのメモは消さない）。

ユーザーの確認結果（2026-09-24）と対応：

- Wi-Fi（XMBから起動、本体の接続画面）と画面の見た目：問題なし。
- **決定ボタン**：ユーザーは×決定の設定。OSKは×決定なのにアプリは○決定で混乱する → 本体設定（`PSP_SYSTEMPARAM_ID_INT_UNKNOWN`＝9、0なら○、1なら×）を読んで決定/戻るを入れ替え、画面下の案内のアイコンも合わせた（`read_button_setting`）。
- **OSKで漢字変換できない**：tilefinchと同じく漢字モードを明示しても、自作プログラムから開いたOSKでは変換が出ない（原因は不明。本体側の変換機能を呼び出す条件が分かっていない）。→ アプリ側で変換する：OSKで打った文字にひらがなが含まれていたら、Googleの変換サービス（`https://www.google.com/transliterate?langpair=ja-Hira|ja&text=`、文節ごとに最大5候補）とYouTubeの検索候補（`suggestqueries.google.com/complete/search?client=firefox&ds=yt`）から候補一覧を出し、選んで検索する（`native/app/kanji.c`、`choose_candidate`）。△で候補をOSKに戻して修正できる。
- **検索件数が少ない**：L/Rでページ送り（`tilefinch_token`付きの続きのURLを読む）。続きの応答は5〜20本とばらつくので、12本未満なら次の続きを2回まで読み足す。1ページ最大20本（`YOUTUBE_LITE_MAXIMUM_RESULTS`を12→20）。
- **検索が0件になることがある件**：原因はYouTubeの新形式のカード（`lockupViewModel`）。6回に1回ほど、応答の動画がすべてこの形式で来て、tilefinchの解析部品が読めずに0件になっていた。`youtube_lite.c`に読み取り（`lite_parse_lockup`）を追加。両形式の保存応答を`native/tests/fixtures/api-search-*.html`に置いた。
- **関連動画**：一覧・マイリスト・再生中に□。動画ページ（next API）の「Up next」を読む。上限を6→12本に（`YOUTUBE_LITE_WATCH_RECOMMENDATIONS`、動画ページのキャッシュ上限を16→24KB、tilefinchのテストも合わせた）。一覧は4段まで積み、戻るで前の一覧に戻る。
- 自動試験（変換→検索→次ページ→前ページ→関連→戻る→再生といいね→マイリスト）は実機でPASS。

**注意**：`YOUTUBE_LITE_MAXIMUM_RESULTS`と`lockupViewModel`の変更はブラウザ版にも効くが、ブラウザ版はまだ作り直して入れていない（1ページ20本にしたときのブラウザ版のメモリは未確認）。

**再生中に画面が暗くなる件**：tilefinchには`scePowerTick`が一度も無く、ボタンを押さずに見ていると本体設定の時間で暗くなり、自動スリープもしていた。`psp_media_session.c`の`psp_media_advance`で、再生中・読み込み中は1秒ごとに`scePowerTick(PSP_POWER_TICK_ALL)`を呼ぶようにした。ブラウザ版（`KOMI_TUBE/slot-a/EBOOT.PBP`）にも入れてPSPLink経由で差し替え済み。

## 検証の方針（2026-09-24、ユーザーと合意）

- **検証は実機が基本**。PSPLinkで`ld host0:/…prx`→ログは`host0:`に届くので、手間はPPSSPPと変わらず、メモリ・デコード・通信の結果が確実。
- 純粋なロジック（`yt/`のJSON解析とフォーマット選択、MP4のdemux）は**Mac上のテスト**で回す。実機で失敗した応答はfixtureとして保存し、Macで再現して直す。
- PPSSPPは原則使わない（PSPがつながっていないときの起動確認程度）。
- 試験EBOOT/PRXは必ず時間切れで自動終了させる（固まると電源長押しをユーザーに頼むことになるため）。

## ユーザーの追加要望（2026-09-24）

- **漢字で検索できること（ぜひ、と強い要望）**。段階3の検索画面で入れる。PSPのOSK（`sceUtilityOskInitStart`）の日本語入力で漢字変換まで使えるかを実機で確かめ、使えなければ自前の変換を検討する。表示側は結果のタイトルを日本語で出す必要があるので、intraFontでファームウェアの`flash0:/font/jpn0.pgf`を使う。検索クエリはUTF-8でURLエンコードする。
- **動画のダウンロード（優先度は低い）**。段階4以降。再生と同じ`yt/resolver`で得たURLから、音声と映像をメモリースティック（`ms0:/PSP/VIDEO/`など）へ保存し、オフラインで再生できるようにする。容量と途中再開（Range）に注意。
- 実機での現状（MEMSIZE=1にしたtilefinch版）：動画5本ほど続けて見て止まらなかった（ユーザー報告）。エラーログも出ていない。

## 実機で確定したこと（PSP-3000、kuKernelGetModel=8、FW 6.61）

ログと集計は`docs/field-results/2026-09-24-memprobe-v1/`と`-v2/`。

- **使えるメモリはMEMSIZE=1のときだけ49.6MB**。MEMSIZE=0と2は22.6MB（このCFWでは2は拡張されない）。
- **今のkomi-tubeはMEMSIZE=2だった** → 実機では22.6MBで動いていた。`scripts/package.py`で1に書き換えるよう変更済み（実機未確認）。
- **MEは拡張領域も読み書きできる（AAC）**：ctrl・作業領域・入力・PCMを通常領域、揮発メモリ、入出力だけ高位、すべて高位の4通りに置き、24フレームとも成功、出力も一致した。tilefinchの`media_backend_psp_pool.h`冒頭のコメント（MEは`0x0A000000`以上を読めない）は、少なくともAACでは当てはまらない。
- AACの作業領域（`sceAudiocodecCheckNeedMem`の結果、ctrl[4]）は**100744バイト**。tilefinchのプールは64KBを想定しているため、実機ではヒープにフォールバックしているはず。
- AV・netモジュールの消費：avcodec 0、mpeg_vsh 約43KB、netモジュール 約432KB、sceNet初期化 約168KB。
- 揮発メモリ（`0x08400000`、4MB）はユーザーモードの`sceKernelVolatileMemTryLock`で取得でき、MEからも読める。
- デコーダーを10回開閉しても漏れは0。

確定したメモリ割り当て（MEMSIZE=1）：

| 領域 | 範囲 | 大きさ |
|---|---|---|
| プログラム・newlib・モジュール | `0x08800000-0x089C5B00` | 約1.8MB（調査EBOOTの場合。本番は大きくなる） |
| 空き（低位） | `0x089C5B00-0x08C00000` | 約2.2MB |
| ME専用領域 | `0x08C00000-0x09200000` | 6MB（4MB境界。先頭にDDR 2MB） |
| 予備 | `0x09200000-0x09300000` | 1MB |
| 汎用アリーナ | `0x09300000-0x0BA80000` | 39.5MB（うち26.5MBが拡張領域） |
| メインスレッドのスタックなど | `0x0BA80000-0x0BB00000` | 約0.5MB |

ME専用領域は保険として低位に固定したままにする（低位には余裕がある）。本番はプログラムが大きいので、4MB境界の候補が`0x09000000`へずれる可能性を考慮する（調査EBOOTと同じく、4MB境界ごとに確保を試す方式にすること）。

## 未確認・注意点

- `./scripts/build.sh`はローカル（Mac）で通ることを確認済み（2026-09-24）。その際、ランチャーが`sctrlKernelLoadExecVSHMs2`で起動するブラウザ本体`slot-a/EBOOT.PBP`がMEMSIZE=2のままだったため、`package.py`でこちらも1に書き換えるよう修正した（CFWは起動するEBOOTのSFOで区画の大きさを決める）。この版をメモリースティックに入れ済み、実機未確認。
- MEMSIZE=1にしたkomi-tubeで「検索→再生→関連→再生」が改善するか（HANDOFF.mdの未確認項目もまだ残っている）。
- PPSSPPの限界：MEMSIZE=2は24MB扱い、`sceAudiocodecCheckNeedMem`はサイズを書かない（調査EBOOTは16KBとみなして続行する）、`mpeg_vsh`が無い（kubridgeも無い）、MEのメモリ制約を再現しない。デコードの成否は実機でしか分からない。
- ビルドの注意：PSPのEBOOTで`-lpsputility -lpspnet_inet -lpspnet_resolver`をLIBSに書くと、ツールチェーンのspecsがlibcの後に同じライブラリを再度リンクし、importスタブが分断される（`psp-fixup-imports`が "stubs out of order" と警告する）。これらはspecsに任せ、LIBSに書かない（`native/memprobe/Makefile`参照）。

## 実機の自動化（次にやる準備）

ユーザーの手間を減らすため、PSPLinkで実機試験を自動化する予定。未検証なので、最初に一度動くか確かめる。

- Mac側で使う道具（`pspsh`、`usbhostfs_pc`）はpspdevのSDKに同梱されている（Linux版で確認。macOS版も同梱の見込み）。
- PSP側のPSPLink本体は [pspdev/psplinkusb](https://github.com/pspdev/psplinkusb) からビルドするか、配布物を使う。CFW 6.61で動くかは未確認。
- 使い方の見込み：`usbhostfs_pc <dist のフォルダ>`でMacのフォルダを`host0:`として見せ、`pspsh`からEBOOTを起動する。ログを受け取り、スクリーンショットは`scrshot`コマンドで撮る。これでメモリースティックへのコピーが要らなくなる。
- ボタンの遠隔操作と画面の転送が要るなら（段階3）RemoteJoyLite。自作のEBOOTは、試験の手順をスクリプト化して内蔵すれば、遠隔操作はほぼ要らない。
- ローカルで準備したこと（2026-09-24）：
  - Mac：`brew install libusb`済み。`tools/pspdev/bin/`の`pspsh`と`usbhostfs_pc`がそのまま動く。
  - PSP：PSPLink v3.2.1にtilefinchのパッチ2つ（HOMEでXMBへ戻る、再生中でも安全な`scrshot-user`）を当ててビルドし、`ms0:/PSP/GAME/PSPLINK/`に入れた。作り直すときは`git clone --branch v3.2.1 --depth 1 https://github.com/pspdev/psplinkusb.git <dir>`のあと`PSPDEV=$PWD/tools/pspdev vendor/tilefinch/scripts/build-psplink-home-exit.sh <dir> build/psplink`。
  - `scripts/psplink.sh ready`／`scripts/psplink.sh exec '<pspshのコマンド>'`：tilefinchの`psplink-shell.sh`を呼ぶ薄い入口。`host0:`は`dist/`。PSP無しで`ready`を実行し、`usbhostfs_pc`が起動してPSPを待つところまで確認済み。
  - CFWは**6.61 PRO-C**（ユーザー確認済み）。
- **PRO-CでのPSPLinkは動作確認済み（2026-09-24）**：`scripts/psplink.sh ready`で接続、`ver`は`PSPLink v3.2.1`。`HOST_ROOT=build/memprobe/m1 scripts/psplink.sh exec 'ld host0:/memprobe.prx'`で調査PRXをMacから直接起動でき、ログは`argv[0]`の隣＝`host0:`（Macの`build/memprobe/m1/`）に書かれた。PSPLink内で動かしても空きは**49.6MB**（MEMSIZE=1のEBOOTと同じ）、AAC試験・開閉10回とも実機単体と同じ結果。プログラムが`sceKernelExitGame`で終わってもPSPLinkは残る（`resetonexit=1`）。
  - よって段階1以降の試験EBOOTは、**PRXも出力してPSPLinkから`ld host0:/…prx`で動かす**のを標準にする（memprobeの`Makefile`と同じ）。ログは`argv[0]`の隣に書く。スティックへのコピーは配布用の最終確認だけにする。
- tilefinch本体に実機自動化の一式がある：`vendor/tilefinch/docs/engineering/PSPLINK_DEV_LOOP.md`、`vendor/tilefinch/scripts/psplink-shell.sh`（`usbhostfs_pc`の起動と時間制限付きの`pspsh`実行）、`tools/psplink-loop`（EBOOTを直接`ld`できないCFW向けにLoadExecする小さなPRX）。PSP-3000＋ARK-4で使われていたもの。これを流用する。
- 注意（同文書より）：PSPLink v3.2.1の`scrshot`はMEのファームウェア領域と重なるバッファを使うため、再生中に撮るとAVC/AACがタイムアウトする。再生中の画面はEBOOT自身で撮ること。
- 目標は、ユーザーの作業を「USBでつなぐ → PSPでPSPLinkを起動する」だけにすること。以降の転送、起動、ログ回収、判定はClaude Codeが行う。

PSPLinkが使えない場合は、今までどおり`./scripts/field_sync.sh install`でコピー → ユーザーが起動 → `./scripts/field_sync.sh collect`で回収。

## このブランチで追加したもの

| パス | 内容 |
|---|---|
| `native/memprobe/` | 段階0の調査EBOOT（`main.c`、`Makefile`、`gen_aac_fixture.py`）。現行はv2 |
| `scripts/build_memprobe.sh` | MEMSIZE 0/1/2の3種類をビルドし、`dist/field-kit/PSP/GAME/KOMI_PROBE_M{0,1,2}`に置く。Linuxでは`PSPDEV=<pspdev-ubuntu展開先>/pspdev`を指定 |
| `scripts/field_sync.sh` | `install`（komi-tubeと調査EBOOTをメモリースティックへ。`data/`は上書きしない）／`collect`（ログを`field-logs/<日時>/`に集め、スティック上は`.old.txt`に改名し、集計を出す） |
| `scripts/memprobe_report.py` | memprobeのログをMarkdownの表にする |
| `scripts/package.py` | EBOOTのMEMSIZEを1に書き換える処理を追加 |
| `docs/FIELD-TEST.md` | 実機テスト1回分の手順と、全体の持ち出し予定 |
| `docs/field-results/` | 実機ログ（v1：全3種類、v2：m1） |
