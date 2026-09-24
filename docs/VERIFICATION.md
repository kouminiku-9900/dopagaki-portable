# 検証記録

このプロジェクトの実行経路は `PSP → PSPのWi‑Fi → YouTube / googlevideo.com` です。動画再生時にPC、USB中継、ローカルHTTPサーバー、クラウド変換は使いません。

確認済み:

- Tilefinchの固定コミットを取得し、PSPDEV SDKと依存ソースを固定ハッシュで準備した。
- macOS上のYouTube resolverで、実際のYouTube URLから映像と音声のURLを取得し、MP4の範囲読み取りとサンプル走査を最後まで完了した。
- PSPクロスコンパイルで `psp-browser-script` と安定ランチャーを生成した。
- `dist/YouTube-for-PSP.zip` にEBOOT、WASM、証明書、フォント、依存ライセンスを含めた。
- PPSSPP 1.20.4の隔離設定でWLANを有効にし、`https://m.youtube.com/` のHTTPSページをPSP EBOOTから取得してHTTP 200と終了処理まで確認した。
- PPSSPP上のYouTube経路で、実動画の360p映像（itag 133）と音声（itag 140）の解決、googlevideoの実データ取得まで確認した。ホスト側resolverでも映像・音声の全範囲走査を完了した。
- PPSSPPのレンジ検証では映像433,081 bytes、音声309,288 bytesを取得した。ただし小さい検証動画のため境界越え判定は完了扱いにならず、実機での長時間ストリーミングの証明にはしていない。
- ホスト側のTilefinch Releaseテストは大半が通過した。`tilefinch-fetch-redirect-tests` はこの環境の並行トランスポート競合で失敗したため、完全成功とは報告しない。

2026-09-21 実機ログ（関連動画を開いた後の再生失敗）:

- 詳細ページから関連動画を開くと `player: response exceeded shared memory budget`。詳細ページは動画ページのHTML（700〜760KB）と解読用バッファを使っており、その後の動画情報（約250〜384KB）の連続領域が取れなかった。
- 対策：YouTubeからの取得をすべて小さなJSONに切り替えた。ホームは通信なし、検索は検索API（約180KB）、詳細・関連動画は次API（160〜215KB）を使う。組み込みの公開Webキーとクライアント版数を使い、APIが断られた場合だけHTMLに戻る。Mac上の実通信とPPSSPPで、検索・詳細ページの取得（応答元がAPIサーバー）と、その後の動画URLの解決を確認した。
- エラー履歴に、アプリのメモリで確保できる最大の連続領域（heap-largest）を記録するようにした。

2026-09-21 実機ログ（検索ページの取得失敗）:

- 特定の検索語で毎回 `YouTube page fetch failed: stream consumer rejected background response body`。検索結果HTMLは展開後約580KB（中身のデータが\x22形式でエスケープされて膨らんでいる）で、倍々で広げる受信バッファが1MBに広がる瞬間の確保に失敗していた。
- 上流には検索APIを直接使う経路があったが、接続情報のキャッシュがCookieの指紋に縛られており、通信層が応答Cookieを後から反映するため毎回無効になって、実際には使われていなかった。キャッシュをCookieの指紋から切り離し、最初の検索もAPIで取るようにした。Mac上の実通信で、検索1回あたり受信570KB→178KB、受信バッファの最大1MB→256KBになり、結果12件と次ページが同じように得られることを確認した。APIが拒否した場合は従来のHTMLに戻る。
- ページ移動（検索・リンク・戻る）の前にも、キャッシュ類を一括で解放するようにした。受信バッファの確保に失敗したときは、必要バイト数をエラー文に残すようにした。

2026-09-21 実機ログ（専用メモリ確保後の2本目）:

- 2本目で `player: response exceeded shared memory budget`。動画情報（player API応答、約250KB）の受信バッファが取れなかった。
- 対策：動画を選んだ瞬間に、ページ通信・先読み解決・接続の事前準備を止め、フォント読み込み途中のデータ・JavaScriptの余剰・HTTPキャッシュ・描画タイルを一括で解放するようにした。受信バッファは倍々で広げる方式をやめ、384KBを一度に確保して、足りなければ256KBずつ広げるようにした。実機未確認。

2026-09-21 実機ログ（2本目以降の再生失敗）:

- `tilefinch-error-history.txt` で、2本目以降の再生失敗は全件 `PSP aligned codec allocation failed: missing=ddr, pool=none` だった。起動時の動画用専用メモリ（約5.3MB）が「heap 24MB＋専用領域分」という条件で見送られており、4MB境界に揃える必要がある2MBのDDR領域を、細切れになった通常メモリから毎回取ろうとして失敗していた。
- 対策：起動時の条件をheap 3MB＋専用領域分に下げ、それでも確保できない場合は最初の再生時に確保して以後保持するようにした。PPSSPP（heap約9MB）で起動時に確保され（base=0x09400000）、検索ページの表示と動画URLの解決が引き続き動くことを確認した。実機での2本目以降の再生は未確認。
- ホスト上で2時間の動画（映像50MB＋音声117MB）を256KB単位の分割取得で最後まで読み切った（失敗0回）。

2026-09-21 実機ログ（PSP-3000、前回ビルド）:

- `data/tilefinch-last-error.txt` は `stage=network-association detail=waiting-ip apctl=0`、45秒でタイムアウトしていた。画面は最初の文言（PREPARING WI-FI）のまま更新されていなかった。原因は、起動時の事前接続（warmup）が固定プロファイル1で `sceNetApctlConnect` を呼んでおり、公式ダイアログの経路を通っていなかったこと。
- 対策として、公式接続ダイアログをブラウザエンジン起動前の `main` 冒頭で実行するよう変更した。ネットワーク層は接続済みのサービスを引き継ぎ、再初期化しない。PPSSPPでダイアログは1回だけ表示され、ブラウザ側は6msで接続済みを検出し、YouTube検索がHTTP 200で返った。実機での結果は未確認。

2026-09-21 追加確認（Wi-Fi接続の作り直し後）:

- 起動時の接続を、固定プロファイルへの `sceNetApctlConnect` 直呼びから、PSP標準の接続ダイアログ（`sceUtilityNetconf`、CONNECTAP）に変更した。PPSSPP 1.20.4でダイアログ経由の接続成功（APCTL GOT_IP）→ `https://m.youtube.com/` HTTP 200 → 正常終了を確認した。
- PPSSPPで `https://m.youtube.com/results?search_query=psp+game` の検索結果を解析し、先頭動画の事前解決が始まることを確認した。
- PPSSPPで検索結果の動画を再生経路に渡し、360p映像（itag 134, avc1.4d401e）と音声（itag 140, AAC-LC）の解決まで確認した。その先はPPSSPPに `mpeg_vsh` が無いため未確認。
- STARTの検索はGoogle/BingではなくYouTube検索（`m.youtube.com/results`）に送るよう変更した。ホームのWikipediaタイルを削除した。

未確認:

- 実機のPSP-3000での起動・Wi‑Fi接続・音声付き再生。手元に実機がないため、ここを成功とは報告しない。
- PPSSPPでの音声付き実動画のデコード・再生。PPSSPPには実機の `mpeg_vsh` がないため、YouTube解決後のデコーダ初期化で止まる。実機のPSPファームウェア経路ではこのモジュールが必要になる。
- PSP-2000 / PSP go。開発元の想定対象だが、このプロジェクトでの実機確認はない。
- PSP-1000。32MB構成のため対象外。

現行のYouTube制約:

- 既定画質は360p、設定から240pに下げられる。
- PSPファームウェア経路が扱えるH.264 Baseline/MainとAAC-LCを優先する。
- 年齢制限、会員限定、ログイン必須、地域制限、追加のProof-of-Origin tokenが必要な動画は再生できない場合がある。
- YouTubeのクライアント仕様変更で再生可能範囲が変わる。resolverは失敗時にエラーを返し、PC側の代替経路へ切り替えない。

実機テスト時は、PSPのネットワーク設定に保存した接続プロファイル番号を `data/boot.cfg` の `network_profile` と合わせ、最初は短い公開動画を360pで再生する。通信ログや期限付きgooglevideo URLを公開しないこと。
