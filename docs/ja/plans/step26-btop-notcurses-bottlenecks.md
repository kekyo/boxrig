# btop と notcurses-demo で測った boxrig のボトルネック

2026年11月20日、`develop` の `3037fc06` を Intel Iris Xe の X11 実機で測定しました。最も時間がかかったのは、notcurses-demo の多色文字デモ grid を GTK3 Cairo で描く処理です。デモの終了まで26.10秒を要し、追加計測では端末CPU時間の87%を描画関数が占めました。btop では OpenGL が変化の少ない行でも字形と配置を計算し直しています。画像デモ xray では、画像を受信して復号する処理と、画面に描く処理の両方に時間がかかりました。

## 実アプリの速度

同じX11画面で、付属の端末 `boxrig_term` のネイティブX11版とGTK3版を動かしました。試行ごとに端末を起動し、118列×45行、1セル7×14画素であることを確認しました。描画構成の順番を入れ替えて各条件を3回ずつ測りました。この節の表は各項目の中央値です。「端末CPU」はboxrigプロセスの全スレッドが使ったユーザー時間とシステム時間の合計です。デモ自身のCPU時間も全スレッドの合計で、複数のスレッドが同時に動くと経過時間を上回ります。

| 描画構成 | grid 実行時間（範囲） | grid 端末CPU | xray 実行時間 | xray 送信枚数（範囲） | xray 端末CPU | xray デモ自身のCPU |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| X11 OpenGL ES | 6.73秒（6.03〜8.08） | 6.27秒 | 15.58秒 | 274枚（247〜290） | 5.63秒 | 26.23秒 |
| GTK3 OpenGL | 12.10秒（11.54〜12.22） | 11.49秒 | 15.55秒 | 223枚（213〜266） | 5.43秒 | 25.96秒 |
| GTK3 Cairo | 26.10秒（23.72〜27.48） | 25.73秒 | 15.57秒 | 260枚（237〜279） | 6.02秒 | 25.52秒 |

notcurses-demo 3.0.7 をboxrigの疑似端末（PTY）で直接起動し、grid は `-c -d 0.03 g`、xray は `-c -d 0.03 x` で乱数種と待ち時間倍率をそろえました。grid は各試行で約119 MiBを書き出しました。デモ自身が報告した書き込み時間は、X11 OpenGL ESで6.05秒、GTK3 OpenGLで11.29秒、GTK3 Cairoで25.51秒です。xray は15秒の動画再生に間に合わないフレームを間引くため、notcurses が報告した画像送信件数（`Bmap emits`）も表に載せました。[notcurses 3.0.7 の xray](https://github.com/dankamongmen/notcurses/blob/v3.0.7/src/demo/xray.c#L93-L123)

| 描画構成 | btop 2秒更新の端末CPU使用率 | 100 ms更新の端末CPU使用率 | 100 ms更新の端末CPU時間／受信更新 | 100 ms更新の btop 自身のCPU使用率 |
| --- | ---: | ---: | ---: | ---: |
| X11 OpenGL ES | 0.75% | 12.40% | 12.63 ms | 12.24% |
| GTK3 OpenGL | 0.92% | 13.83% | 14.07 ms | 12.24% |
| GTK3 Cairo | 0.83% | 17.15% | 17.46 ms | 12.33% |

btop 1.3.0 の表示はCPU、メモリー、ネットワーク、プロセスの4パネルで、時計は非表示です。試行ごとに専用の設定を用意し、出力を `script -q -e -f` で記録しました。記録が10,000バイトを超えてから4.25秒待ち、続く12秒間に届いた更新完了の制御シーケンスを数えました。受信件数は2秒設定で6回、100 ms設定で118〜119回です。CPU使用率は論理CPU 1個を100%とします。端末と btop のCPU時間を各プロセスの `/proc/PID/stat` から10 ms単位で読みました。

## grid の描画

実アプリを2回ずつ追加で動かし、関数の入口と出口で経過時間と呼び出しスレッドのCPU時間を読みました。GTK3 Cairo は各試行で741回と745回描画し、描画関数に21.74秒と21.94秒を費やしました。いずれも端末CPU時間の87%です。入力を解析する関数は1.75秒と1.79秒でした。

記録したgridの24更新を1更新につき1回描くと、既定設定では字形を一時画像に描いて画面に合成する処理が122,265回ありました。描画CPU時間は1更新平均30.67 msです。字形の輪郭をグレースケールに固定すると、輪郭をマスクとして再利用でき、一時画像の生成は408回、描画は14.96 msでした。値は3試行の中央値で、3組すべてで描画時間が短くなりました。描画方式が変わるため、両設定の画素ハッシュは24更新すべてで異なりました。[字形の描画と合成](../../../src/pangocairo/renderer.c#L4356)・[固定入力の計測](../../../tests/performance/cairo_grid_replay.c#L65)

Callgrindで同じ入力の描画関数だけを調べると、実行命令の77.67%が、整形済みの字形を描く `draw_shaped_item_segment` とその呼び出し先に集中しました。計測範囲は準備の1回を含む25回の描画です。このうち `pango_cairo_show_glyph_string` とその呼び出し先は全体の43.12%でした。[字形の描画単位](../../../src/pangocairo/renderer.c#L4486)

ネイティブX11では、PTYからの読み取りと入力解析を含むセッション処理に2.80〜3.02秒、OpenGLのフレーム準備に2.77〜3.22秒のCPU時間を使いました。バッファを交換する `eglSwapBuffers` 内の経過時間は、357〜387回の合計で0.07〜0.08秒です。このX11版は連続出力の描画を最長8 ms待ち、その間に届く変更をまとめます。GTK3 OpenGLでは表示先への転送が587〜731回、フレーム準備のCPU時間が5.10〜7.40秒でした。[X11の描画予約](../../../examples/boxrig_term/x11/output_frame.h#L22)・[X11の表示処理](../../../src/x11_opengl/component.c#L1077)・[GTK3の表示処理](../../../src/gtk/gl_surface.c#L365)

## btop の行整形

OpenGLの行整形では、行の文字を集め、Pangoで書体と字形を決め、各セルへの配置を計算します。100 ms設定のbtopを各構成で2回ずつ追加計測しました。受信1更新あたり、行整形にX11で平均3.91 ms、GTK3で平均4.16 msかかり、それぞれ端末CPU時間の32%と29%を占めました。両OpenGL構成で入力解析は0.92〜0.97 ms、自動URL認識は1.38〜1.47 msでした。GTK3 Cairoでは描画関数全体が平均13.68 msを占めました。[行の変更判定](../../../src/opengl/renderer.c#L560)・[OpenGLの行整形](../../../src/opengl/text_provider.c#L1184)・[URL認識](../../../examples/boxrig_term/common/text_policy.c#L381)

記録した同じ24更新を両OpenGL構成で再生すると、前の画面から文字やスタイルが変わったのは延べ4,880セルでした。どちらの構成も延べ569行を整形し直しました。対象の67,142セルのうち、62,262セル（92.7%）は前の画面と同じでした。文字の変化が数字だけでスタイル境界も変わらなかったのは、569行のうち16行です。[変更セルと整形範囲の計測](../../../tests/performance/opengl_update_probe.c#L178)

## xray の画像処理

GTK3 OpenGLで記録した217更新から、末尾の23画像と最後の消去を各構成で1回ずつ再生しました。各画像の寸法は826×464画素で、色と透明度（RGBA）を文字列に符号化したデータは1枚2,044,076バイトです。復号したRGBAのハッシュと配置を照合し、ウィンドウ上では不透明な画素の色と最後の消去を確認しました。表は最初の画像と最後の消去を除く22枚について、1枚あたりの処理スレッドの平均CPU時間を示しています。

| 描画構成 | 入力全体 | うちBase64復号 | 描画 | テクスチャ転送指示 |
| --- | ---: | ---: | ---: | ---: |
| X11 OpenGL ES | 9.26 ms | 4.94 ms | 8.35 ms | 1.43 ms |
| GTK3 OpenGL | 8.20 ms | 4.27 ms | 7.73 ms | 1.48 ms |
| GTK3 Cairo | 7.69 ms | 3.99 ms | 7.03 ms | — |

どの構成でも入力処理と描画の両方が1枚につき7 msを超えました。入力CPU時間の約半分を、Base64で符号化された画像データの復号に使っています。OpenGLの転送時間は描画時間に含まれ、CPUが転送を指示する間を測った値です。Cairoの描画には画像の変換1.64 msと合成0.76 msが含まれます。[画像データの復号](../../../src/core/graphics_protocol.c#L5757)・[計測区間](../../../tests/performance/image_phase_probe.c#L155)・[画像と画素の照合](../../../tests/performance/check_xray_replay.mjs#L121)

## 画面に反映するまでの時間

btopは先頭42更新、gridは先頭8更新で画面を準備し、続く24更新を計測しました。各構成に同じ入力を1更新ずつ送り、送信開始から、目印を含む同じ画面をX11ウィンドウで連続2回読めるまでの時間を測りました。送信間隔は100 msです。前の更新の確認にそれ以上かかった場合は、確認後に次を送ります。表のp95は、最初を除く23更新の95パーセンタイルを試行ごとに求めた値です。3試行の中央値と範囲を示します。

| 描画構成 | btop の画素反映p95（最小〜最大） | grid の画素反映p95（最小〜最大） |
| --- | ---: | ---: |
| X11 OpenGL ES | 50.8 ms（50.1〜53.4） | 54.5 ms（53.8〜57.0） |
| GTK3 OpenGL | 72.1 ms（65.0〜89.5） | 85.1 ms（69.7〜93.6） |
| GTK3 Cairo | 77.5 ms（47.7〜78.2） | 112.2 ms（103.2〜124.2） |

全18試行で24更新の順序を画素と録画時刻から確認しました。GTK3 Cairoのgridは、3試行ともp95が100 msを超えました。この遅延には、端末の入力処理や描画のほか、画面を読み直すまでの待ち時間と2回の読み取りが含まれます。[入力送信](../../../tests/performance/replay_marked_capture.mjs)・[画素の観測](../../../tests/performance/frame_latency.c#L257)・[録画時刻と画素の検証](../../../tests/performance/check_frame_latency.mjs)

## 計測環境と再生方法

Ubuntu 24.04.5、Core i7-1370P、Intel Iris Xe（Mesa 25.2.8）、内蔵90 Hz画面、電源プロファイル `low-power` を使いました。boxrig は GCC 13.3.0 の `-O2 -g`、GTK3 は3.24.41、Pango は1.52.1、Cairo は1.18.0です。フォントは DejaVu Sans Mono、`TERM=xterm-256color`、`LC_ALL=C.UTF-8`、GTKの表示倍率は1です。URLの自動認識は btop で有効、notcurses で無効にしました。

各入力の全バイトを、描画を外して端末コアに3回ずつ再生しました。ファイル読み取りと端末応答の確認を含むCPU時間の中央値は、btop が3,560,925バイトで0.084秒、grid が124,487,793バイトで1.478秒、xray が445,087,291バイトで0.611秒です。各入力で3回の応答・状態の検査値が一致しました。[端末コアでの再生](../../../tests/performance/notcurses_replay.c#L106)

この作業環境の `artifacts/perf-speed-20261120-I16zmq/` に、試行別の数値と集計（`summary.json`）、環境と実行ファイルのハッシュ（`environment.txt`）を保存しました。実アプリのログは `live/`、関数別の時間は `phases/`、固定入力は `inputs/` にあります。`latency/` と `details/` の各画素計測には、動画 `video.mkv` と元の画像を格納した `frames.tar.zst` があります。圧縮後の画像は元ファイルと照合済みです。実アプリの起動に使った `live.mjs`、`child.sh` と、追加計測に使った `detail.sh` も同じ作業ディレクトリにあります。

リポジトリ直下で、記録した入力の再生と100 ms更新のbtopの再測定ができます。最後のコマンドはX11画面を使い、結果を `recheck/1-x11-gl-btop100/` に保存します。

```sh
work=artifacts/perf-speed-20261120-I16zmq
make -j4 notcurses-evaluation-tools build/tests/performance/cairo_grid_replay build/boxrig_term/x11/boxrig_term
build/tests/performance/cairo_grid_replay "$work/inputs/grid"
build/tests/performance/notcurses_replay "$work/capture-xray/output.bin" 45 118 7 14 script
node "$work/live.mjs" x11-gl btop100 1 recheck
```

別のX11実機では、付属スクリプトでgridの入力を採取して、同じ24更新をCairoで再生できます。

```sh
out=$(mktemp -d)
GDK_SCALE=1 GDK_DPI_SCALE=1 bash tests/performance/run_notcurses_evaluation.sh \
  build/boxrig_term/gtk3/boxrig_term opengl 856 690 0 g 0.03 "$out/capture" 0 0 45 118
node tests/performance/extract_capture_updates.mjs grid "$out/capture/output.bin" "$out/grid"
build/tests/performance/cairo_grid_replay "$out/grid"
```

## テスト結果

計測後、全体テストの `make -j1 test` は `test-win32-core-artifacts` で停止しました（終了コード2）。64ビット版の画像テスト `test_graphics.exe` が呼ぶ `msvcrt.dll` の `strstr` が、対応OSを確認するためのインポート許可リストに未登録でした。直前のスクロール履歴の検証にも同じ失敗が記録されています。[画像テストでの呼び出し](../../../tests/test_graphics.c#L3300)・[監査](../../../tests/win32/check_core_pe.sh#L37)・[前回の記録](plan25-scrollback-compression.md#全体テストと基準版との照合)

今回の計測に使った入力再生と画素観測の試験は `make -j1 test-notcurses-replay test-frame-latency` で通りました。全体テストのログは計測ディレクトリの `full-test.log`、個別試験のログは `measurement-tests.log` です。
