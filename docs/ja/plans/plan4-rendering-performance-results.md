# plan4の実装と検証記録

[実装計画](plan4-rendering-performance.md)の順に作業を進めています。
作業ブランチは`feature/rendering-performance`、開始時点は`c389357`です。
elder-termsと外部依存の固定参照は変更していません。

## 段階1の成果物

[比較ドライバー](../../../tests/rendering/replay.c)は、core、GTK3 Cairo、GTK3 OpenGLで同じ41更新を処理します。
各更新の認識とイベント消費を完了させてから次へ進みます。
GUIでは合成後の画素を読み、状態番号に対応する色を確認します。
本文全体のSHA-256も記録し、coreとGUIで一致することを確かめました。
画素の読戻しと検証用snapshotは、区間時間と処理量の採取後に行います。
試行全体のプロセスCPU時間には、この検証費用も含まれます。

これは端末APIへ直接入力する比較です。
PTYへの書込みからの遅延や、付属アプリのGSourceの性能を示す値ではありません。
その比較は、認識続行の修正後に改めて行います。

ビルドと既知の入力による確認は次のコマンドです。

```sh
make test-rendering-measurement
make build/text-measurement/measure_text_workloads
build/text-measurement/measure_text_workloads self-test
```

詳細計測版には、snapshotの投影セル数・バイト数と位置表の比較回数を追加しました。
snapshotの寸法測定と実データの書込みを両方数えます。
小さい既知の入力で、計数追加前の失敗と追加後の成功を確認しました。
従来のwork値と既存の計数は維持しています。

描画側では行signature、Pango整形、Cairo描画、完成画像の確保とコピーを分けています。
OpenGL描画、保持画像の転送、GTKの画像取込み、GTK合成にも経過時間とスレッドCPU時間を付けました。
計測区間は重なるため、各区間の中央値を足して全体時間にはできません。
通常の製品ライブラリーには時計の呼出しを入れず、専用objectで計測します。
対照版は通常ライブラリーを使い、詳細計数と区間時計を無効にします。

## 段階1の基準値

実行日は2026年10月17日です。
入力は計画にあるbtop記録で、SHA-256は`5a5a8bceee8f361658f3a91bb1aaf70db2c25f877bf72ce18102ce1e483daaf3`です。
同期出力の119区間から先頭41区間を使用しました。
表示確認用の色を各区間へ追加するため、再生バイト列にはその分が加わります。

環境はGTK 3.24.41、X11、RTX 3090、NVIDIA 615.71.09です。
端末は190列×66行、セルは10×22px、フォントは16DIPのNoto Sans MonoとMonospaceです。
CPU 2へ固定し、周波数は固定していません。
詳細計測と対照の順序を入れ替えて3組実行しました。
最初の更新はwarm-upとして時間分布から外しています。

```sh
bash tests/rendering/run_measurements.sh \
  /tmp/elder-render-profile/btop-host-baseline.output \
  build/test-results/plan4/stage1-final 2
```

18試行すべてが終了コード0でした。
全試行の各41状態で本文ハッシュが一致し、認識は1,967回、7,958,154 workで完了しました。
一致は0件で、認識イベントも残りませんでした。
GUIでは全状態の色を確認し、要求した描画方式と実backendが一致しています。
OpenGLの実FBOはalpha 8bit、単一sample、1900×1452pxです。

各試行の中央値の範囲は次のとおりです。単位はmsです。

| 方式 | 詳細計測の経過時間 | 対照の経過時間 | 詳細計測のCPU時間 | 対照のCPU時間 |
| --- | ---: | ---: | ---: | ---: |
| core | 2.365〜2.385 | 2.280〜2.293 | 2.365〜2.385 | 2.280〜2.291 |
| GTK3 Cairo | 17.784〜19.063 | 17.553〜17.771 | 17.267〜17.432 | 17.281〜17.320 |
| GTK3 OpenGL | 22.626〜27.580 | 23.434〜26.446 | 11.300〜11.343 | 11.223〜11.337 |

coreでは詳細計測によってCPU時間が約3〜5%増えています。
GUIのCPU負荷の大小と、OpenGLで経過時間がCPU時間より長い傾向は対照でも同じでした。
GUIのp95には大きなばらつきがあり、単一試行から小さい差の改善は判断しません。
各試行の中央値・p95・40更新の分布、周波数、実行ファイルのhashは[基準値JSON](plan4-measurements/step1-baseline.json)に保存しています。
生ログ、終了コード、測定時の差分は`build/test-results/plan4/stage1-final/`にあります。

OpenGLの行signatureはCPU時間1.028〜1.036ms、Pango整形は1.585〜1.612msでした。
GTKへの画像取込みは経過時間9.620〜11.464msです。
Cairo描画はCPU時間12.669〜12.776msで、その中のPango整形が2.504〜2.536msでした。
完成画像のコピーは0.644〜0.658msです。
コピーだけを削ってCairoの負荷全体が解消するとは判断できません。

認識では41更新合計で2,056,560セルをsnapshotへ投影し、位置表を7,622,269回比較しました。
1,024行の既存一致を保持して別行を更新する既存ドライバーでは、snapshotが164,000セルを投影しています。
その試行の確保回数は3,102回、測定区間内に確保した領域の同時保持量は最大1,155,658バイトでした。
これらを、後続の境界走査と行再利用の比較基準にします。

## 付属ターミナルの認識停止を再現する

[GSourceの試験](../../../tests/gtk/test_sample_recognition.c)は、実際の付属アプリのdispatch処理とGTK widgetを使います。
表示の初期化を済ませてから、複数budgetを必要とする本文と末尾URLを与えます。
その後は独立したmain contextでアプリのsourceだけを進めます。
GTKの点滅や追加入力による偶然の再開を避け、sourceが実行待ちを失った時点で未完了を検出します。

修正前のGTK3とGTK4は、認識とframeがそれぞれ2回進んだ後に停止しました。
いずれも`more_work=true`のため、完了を要求する検証に失敗します。
これが段階2で使用するREDです。
専用targetの`make test-gtk-sample-recognition`で実行します。
段階2の修正後に全体テストへ登録しました。

試験作成中には、GTK4の初期allocationが完了する前に測定を始める失敗もありました。
それは認識停止の証拠に含めず、表示とlayoutの初期化完了を待つように試験を修正しました。
詳細は`build/test-results/plan4/sample-gtk3-red.log`と`sample-gtk4-red.log`にあります。

## 段階2の成果物

GTK3とGTK4の付属ターミナルに、認識とイベントの未完了状態を保持するフラグを追加しました。
認識だけが残ったdispatchはidle優先度で続行し、frameを要求しません。
その経路ではセッション同期、フォント設定、スクロールバー設定を繰り返さず、認識とイベント通知を進めます。
装飾などによる実際の描画要求は、従来のview通知に従います。

実際のGSourceを独立したmain contextで動かす試験では、次を確認しました。

- 有限の本文と末尾URLは17回の認識再開で完了し、frameは1〜2回でした。
- URLのない本文でも17回で完了し、frameは1回でした。
- 未mapではframeを呼ばずに完了しました。
- 認識済みの96件のイベントは、2回のdispatchで空になりました。
- 認識無効時と完了後は、sourceのready時刻が解除され、prepareでも実行待ちになりませんでした。
- 通常優先度のsourceがidleの認識続行より先に実行されました。
- 未完了のsourceを破棄するとcallbackが残りませんでした。

試験用のPTY子プロセスから250行と末尾URLを送り、その後の出力を止める試験も追加しました。
GTK3、GTK4とも51回の再開で認識とイベント消費が完了しています。
URL装飾を含む画像のdirtyが受理された後にだけ子プロセスへ終了を通知し、実際のサンプルの終了処理も通しました。
GTK3／GTK4の既存sample試験、連続出力中の入力・描画、閉じ忘れた同期出力からの復帰も成功しています。

実行コマンドは`make -j4 test-gtk-sample-recognition`と`build/tests/test_example_text_policy`です。
既存シナリオは`build/tests/test_gtk3_sample`、`test_gtk4_sample`、`test_frontend_sample_x11`の`--continuous-output`、`--sync-recovery`をXvfbで実行しました。
実行ログは`build/test-results/plan4/sample-expanded-green-4.log`、`text-policy-green.log`、`sample-gtk*.log`です。
試験拡張時には、描画の合流を許さずframeを必ず2回とした検証と、認識無効時の通常dispatchまで認識専用と扱った検証が失敗しました。
frameの上限と認識専用経路を検証する形に直し、製品コードへの追加修正は行っていません。

## 認識を完了する付属アプリの基準値

段階1と同じbtop入力を実PTYから送り、各状態の表示と認識・イベント消費を確認してから次へ進めました。
GTK3 OpenGL、X11実GPU、CPU 2、190列×66行、16 DIPの条件です。
3試行とも初期状態と40更新を順に確認し、最後の本文hashも一致しました。
途中のPTY分割でも認識が進むため、APIへ一括投入した段階1とwork量は異なります。

| 指標 | 3試行の中央値の範囲 | 各試行のp95の範囲 |
| --- | ---: | ---: |
| PTY書込み→widget draw終了 | 30.964〜44.701 ms | 62.071〜64.243 ms |
| PTY書込み→60 fps録画での状態検出 | 49.794〜49.822 ms | 82.913〜95.429 ms |

認識CPU時間は試行全体で100.476〜119.658 ms、再開は2,848〜3,364回でした。
認識とイベントキューは全試行で完了し、GL surface描画は各44回です。
周波数の中央値は5.176〜5.200 GHzでした。
この表のdraw終了はGTK合成完了とは異なり、録画の検出時間にも取得・転送の遅延が入ります。
停止していた旧sampleの短い認識時間との性能比較には使いません。

[段階2の基準値JSON](plan4-measurements/step2-sample-baseline.json)に分布、認識結果、PID、実行ファイルhash、生ログを保存しました。
一時ドライバーは`/tmp/boxrig-plan4-stage2/`にあります。
`node /tmp/boxrig-plan4-stage2/build.mjs`で現在のsampleソースと現在の静的ライブラリーを接続し、
`DISPLAY=:1 PROBE_CPU=2 PROBE_FRAME_STATS=1 RENDER_PROBE_LABEL=baseline4- node /tmp/boxrig-plan4-stage2/replay.mjs sample --host`などで実行しました。
過去のelder-terms用共有ライブラリーは読み込んでいません。
詳細測定前の試行と、計測コードのインデント警告を修正したビルド履歴も`build/test-results/plan4/stage2-profile-*.log`へ残しました。

## 段階3の進行

GTKの実表示先を、FBO・寸法・encoding・alpha bit数・sample数・世代の専用記述に分離しました。
GL surfaceはその寸法とencodingからRGBA保持画像を確保し、実際の保持画像だけを公開rendererへ渡します。
この時点ではRGBの受け入れを広げていません。
GTK3／GTK4のRGBA保持画像、差分描画、失敗後の再試行とdirtyの受理、描画方式の設定とcontext故障を確認しました。
`NO_AT_BRIDGE=1 GTK_A11Y=none LIBGL_ALWAYS_SOFTWARE=1 make -j1 test-gtk-gl-surface test-gtk3-rendering test-gtk4-rendering`は成功しています。
ログは`build/test-results/plan4/stage3-refactor-rgba.log`です。

## 残る作業

段階1・2の完了条件を確認しました。段階3〜9は未完了です。
RGB表示、CPU描画の削減、認識の順次走査と行再利用は、計画の順に実施します。
snapshotの分割は、段階7後の負荷を測って着手を判断します。
全体テスト`make -j1 test`、実アプリでの比較、動画・実機確認も最後に必要です。

計測で使うGLib sourceの優先度と寿命は[公式main loop文書](https://docs.gtk.org/glib/main-loop.html)と[2.80.0のAPIコメント](https://github.com/GNOME/glib/blob/2.80.0/glib/gmain.c)で確認しました。
画素の取得は[GTKの公開API](https://docs.gtk.org/gdk3/func.pixbuf_get_from_window.html)と[3.24.41のAPIコメント](https://github.com/GNOME/gtk/blob/3.24.41/gdk/gdkpixbuf-drawable.c)に従っています。
