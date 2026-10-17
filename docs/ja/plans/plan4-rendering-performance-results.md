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

## 段階3の成果物

GTKの実表示先を、FBO・寸法・encoding・alpha bit数・sample数・世代の専用記述に分離しました。
GL surfaceはその寸法とencodingからRGBA保持画像を確保し、実際の保持画像だけを公開rendererへ渡します。
この時点ではRGBの受け入れを広げていません。
GTK3／GTK4のRGBA保持画像、差分描画、失敗後の再試行とdirtyの受理、描画方式の設定とcontext故障を確認しました。
`NO_AT_BRIDGE=1 GTK_A11Y=none LIBGL_ALWAYS_SOFTWARE=1 make -j1 test-gtk-gl-surface test-gtk3-rendering test-gtk4-rendering`は成功しています。
ログは`build/test-results/plan4/stage3-refactor-rgba.log`です。

続いて、実GTK3のalpha 0bitの表示先を拒否することをREDとして確認し、RGB8への転送を追加しました。
保持画像はalpha 8bitのままです。
OpenGLとGLESの両方で、保持画像と転送後の全画素一致、余白のalpha 255、変更行だけの再描画、失敗後の再試行を確認しています。
GTKがRGBからRGBAへbufferを作り直した後も、透明画素を正しく表示できました。
世代0の表示先でdirtyを受理する問題もREDで確認し、無効な世代の転送を拒否するようにしました。

`make -j4 test-gtk-gl-surface`は、MesaのOpenGL 4.5／GLES 3.2、GTK3のRGB／RGBA、GTK4のRGBAで成功しました。
線形とsRGBの保存形式で既知の色を調べ、二重変換やalpha破棄による暗化がないことを確認しています。
この環境のGLESではSRGB8単独のFBOを作れません。
その組合せは非対応として記録し、RGB8とSRGB8_ALPHA8の成功とは区別しました。
GLESの転送条件は[Khronosのリファレンス](https://github.com/KhronosGroup/OpenGL-Refpages/blob/main/es3.0/glBlitFramebuffer.xml)を参照しています。

X11実GPUのNVIDIA RTX 3090でもRGB転送とsRGBを確認しました。
そのコマンドは`DISPLAY=:1 GDK_BACKEND=x11 GDK_GL= NO_AT_BRIDGE=1 GTK_A11Y=none BOXRIG_TEST_GL_API=gl ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 build/tests/gtk3_gl_surface rgb`です。
`NO_AT_BRIDGE=1 GTK_A11Y=none LIBGL_ALWAYS_SOFTWARE=1 make -j1 test-gtk-render-switch`も成功し、
X11と入れ子のWaylandでGTK3／GTK4の切り替え、選択、preedit、故障からの復帰を維持しています。
REDとGREENのログは`stage3-rgb-red.log`、`stage3-generation-red.log`、`stage3-transfer-green.log`、`stage3-nvidia-rgb.log`、`stage3-render-switch.log`です。
試験追加時の変数宣言漏れによるビルド失敗は、製品動作のREDに含めていません。

段階3では表示形式を変えるための接続だけを追加しました。
通常のwidgetはまだRGBAを使い、性能改善は主張しません。

## 段階4の不透明表示と比較

GTK3では、完成画像全体が不透明な構成をRGB表示先へ転送します。
解決済みの透明度がOPAQUE、背景不透明度が255で、アプリの背景色と背景画像を使わない場合に限定しました。
内部の保持画像はRGBAのままです。
選択、スクロールバー、IME未確定文字、余白を含む保持画像の全画素でalpha 255を確認しています。
背景色・画像を含む構成や透明表示はRGBAを使います。

切り替えはGTKのrender開始時に行い、bufferを再接続して表示先の世代を更新します。
RGB表示で不要なCairo CLEARも省きます。
利用方法は[set_has_alpha](https://docs.gtk.org/gtk3/method.GLArea.set_has_alpha.html)、
[attach_buffers](https://docs.gtk.org/gtk3/method.GLArea.attach_buffers.html)と、
[GTK 3.24.41のAPIコメント](https://github.com/GNOME/gtk/blob/3.24.41/gtk/gtkglarea.c)で確認しました。
RGBの転送先が使用できなければ、同じcontextでRGBAを一度再試行します。
内部レンダラー、描画完了処理、contextの失敗は既存のフォールバック規則を維持します。

最初に通常表示で実alpha 0bitを要求し、従来の8bitで失敗するREDを確認しました。
RGB／RGBAの往復、背景色・画像、透明化時の転送失敗、resizeを試験に追加しています。
RGBA再試行の初版では内部fence失敗まで再試行してしまい、既存の切り替え試験が失敗しました。
原因を転送先と内部処理の失敗分類に絞って修正し、同じ試験の成功を確認しました。
`make -j1 test-gtk3-rendering test-gtk-render-switch`と`make -j1 test-gtk-gl-surface`が成功しています。
切り替え試験はGTK3／GTK4、X11／入れ子のWaylandを含みます。
Xvfb／入れ子環境のDRI3・device情報とaccessibility busの診断は残っていますが、実描画・画素・状態の検証は通りました。
コンパイルの警告はありません。

ログは`stage4-opaque-red.log`、`stage4-expanded-green.log`、`stage4-classification-green.log`、`stage4-surface-green.log`です。
最初の分類修正後のコマンドは、二つの試験成功後に誤ったtarget名`test-gl-surface`で停止しました。
正しい`test-gtk-gl-surface`を別途実行して成功しています。
試験追加時のepoxyヘッダー不足によるビルド失敗と、target名の誤りは製品動作のREDには含めません。

性能比較は同じバイナリーでRGBAを強制する診断用wrapを使います。
製品の環境変数や公開設定は増やしていません。
端末だけの構成と、150 msごとに点灯・消灯するGTK部品を併設した構成を比較しました。
後者は点灯・消灯の両方が実際に描画されたことも確認します。

```sh
DISPLAY=:1 sh tests/rendering/compare_presentations.sh \
  /tmp/elder-render-profile/btop-host-baseline.output \
  build/test-results/plan4/stage4-paired 2
```

RTX 3090、NVIDIA 615.71.09、GTK 3.24.41で、順序を入れ替えた3組を詳細計測・対照それぞれで実行しました。
24試行とも実backendはOpenGLで、RGBAは実alpha 8bit、RGBは0bitでした。
41状態すべての本文hashが段階1と一致し、認識の1,967回・7,958,154 work、0一致、イベント消費完了も同じです。
各試行のCPU周波数の中央値は5.169〜5.200 GHzでした。

詳細計測の各試行の中央値の範囲は次のとおりです。単位はmsです。

| 構成 | 形式 | 更新のCPU時間 | GTK画像取込みの経過時間 | 入力から更新完了の経過時間 |
| --- | --- | ---: | ---: | ---: |
| 端末のみ | RGBA | 11.202〜11.297 | 9.440〜11.396 | 23.728〜26.613 |
| 端末のみ | RGB | 7.306〜7.358 | 0.124〜0.171 | 25.019〜29.035 |
| GTK部品が点滅 | RGBA | 11.317〜11.356 | 7.713〜11.642 | 25.260〜26.937 |
| GTK部品が点滅 | RGB | 7.337〜7.472 | 0.136〜0.173 | 24.816〜29.042 |

対照版でもCPU時間はRGBAの11.211〜11.449 msからRGBの7.269〜7.449 msへ減りました。
狙った画像取込みと全体CPU時間の削減が繰り返し確認できたため、この差分を採用します。
ただし経過時間の中央値は多くの組で1〜4 ms増え、表示遅延の改善は確認できませんでした。
詳細計測のp95はRGBAの28.934〜41.454 ms、RGBの28.965〜36.253 msです。
対照ではRGBに48.207 msの試行もあり、遅い側の改善も一律には主張できません。
画像取込み後に残るframe待ちと、実アプリのPTY処理を段階9で比較します。
GPUの完了時間と物理的なモニター表示時間は未計測です。

各試行の中央値、p95、分布、周波数、実行ファイルhashは[比較JSON](plan4-measurements/step4-rgb-comparison.json)に保存しました。
生ログと測定時の差分は`build/test-results/plan4/stage4-paired/`にあります。

## 段階5のCPU描画

段階4のRGB表示でも、行signatureにCPU時間約1.03〜1.06 ms、Pango整形に約1.57〜1.61 msかかっています。
OpenGLの`terminal_row_signatures()`では、色と装飾を含むスタイルのhashを作りながら、同じ情報を行のhashへもう一度加えていました。
ここでは既存のスタイルhashを再利用し、重複する計算だけを除きました。
文字、幅、継続セル、色、装飾、リンク境界は引き続き判定に含まれます。
行の移動やPango layoutの所有権は変えていません。

4K画面の試験で、色の計算量を1セル当たり24バイト以下に制限しました。
修正前は84,392セルに4,050,816バイトを処理してREDとなり、修正後は2,025,408バイトでGREENです。
初回表示、全面色変更、RGB色の変更を同じ上限で確認します。
無変更再提示と再提示だけのdirtyでは再整形せず、1行の文字変更は1行だけを再整形します。
既存の画素試験で色・背景・選択・hover・点滅・IME・フォント・倍率・resize・故障回復も確認しました。
OpenGLとGLESの4K fixture、`test_opengl_colors`、`test_opengl_blink`、`test_decoration_backends`はASan／UBSan付きで成功しています。
ログは`stage5-signature-red.log`、`stage5-signature-green-{gl,gles}.log`、`stage5-{colors,blink,decoration}-green.log`です。

修正前後の実行ファイルを保持し、同じbtopの41状態を各3組、詳細計測と対照の両方で比較しました。
全12試行の本文hash、認識完了、イベント消費、実OpenGL描画が一致しています。
各試行のCPU周波数の中央値は5.155〜5.200 GHzです。

| 計測対象 | 修正前の中央値範囲 | 修正後の中央値範囲 |
| --- | ---: | ---: |
| 行signatureのCPU時間 | 1.033〜1.088 ms | 0.757〜0.772 ms |
| 更新全体のCPU時間・詳細計測 | 7.333〜7.492 ms | 6.965〜7.143 ms |
| 更新全体のCPU時間・対照 | 7.281〜7.404 ms | 6.924〜6.988 ms |

計算量とCPU時間の両方が減ったため、この差分を採用します。
経過時間とp95は試行間のばらつきが残り、表示遅延の改善には数えません。
分布と実行ファイルhashは[段階5の比較JSON](plan4-measurements/step5-signature-comparison.json)に保存しています。
生ログと差分は`build/test-results/plan4/stage5-paired/`にあります。

Pangoの再整形は内容が変わる行で必要であり、色だけの変更と再提示は既存の再利用が働いています。
今回のbtopは画面内の行を書き直す入力なので、端末のスクロールだけを対象にした行移動cacheは追加しません。
Cairo完成画像のコピーは約0.65 msで、Cairo描画約12.7 msより小さい費用でした。
複数bufferの世代管理を増やす変更は、この段階では採用していません。

## 段階6の境界照会

認識用のsnapshot走査に位置表のcursorを追加しました。
1回の処理で比較・前進するのは1項目までで、前進も認識のbudgetに含めます。
cursorは添字だけを保持し、snapshotの作り直しと画面の切り替えで0へ戻します。
任意位置を照会する公開APIと、キャプチャの位置取得は従来の二分探索を維持しています。

4,096文字の長い論理行で、位置表の比較量を文字数に比例する上限へ制限してREDを確認しました。
修正後はこの上限を満たしています。
104万8,576文字の試行も、比較3,145,772回、予算外の行準備0回で完了しました。
単調な照会、同じoffsetの繰返し、末尾への飛び越しを、公開APIの結果と照合しています。
結合文字・全角文字・絵文字の内部、空行、改行、折り返し、resize後の主画面と代替画面が対象です。
terminalを破棄した後も所有済みsnapshotから同じ位置を返すことを確認しました。
既存の認識・キャプチャ試験はbudget 1で中断と再開を繰り返します。

同じ41状態のcore比較を詳細計測・対照で各3組行いました。
本文hash、0一致、認識完了とイベント消費はすべて一致しています。
位置表の比較は7,622,269回から1,569,111回へ減りました。
前進を正しく計上した結果、workは7,958,154から9,007,713、再開は1,967回から2,214回へ増えます。
work値だけを小さくする変更ではありません。
snapshot投影はまだ2,056,560セルのままです。

対照版の更新CPU時間の中央値は、修正前2.215〜2.229 ms、修正後1.842〜1.954 msでした。
詳細計測では2.292〜2.453 msから1.969〜2.275 msで、3組目の差は小さくなっています。
CPU周波数の中央値は5.161〜5.200 GHzです。
比較量の削減と対照でのCPU時間の減少を採用根拠とし、表示遅延の成果には数えません。
分布、実行コマンド、hashは[段階6の比較JSON](plan4-measurements/step6-boundary-comparison.json)に保存しました。

通常のcore全試験、文字処理のself-test、`test-rendering-measurement`、`test-gtk-sample-recognition`は成功しています。
coreの全objectをASan／UBSan付きで別ディレクトリーへビルドし、snapshotと認識関連の試験も成功しました。
その実行ファイルは`build/plan4-sanitized/tests/stage6-focused`、ログは`stage6-focused-green.log`です。
通常のテスト用実行ファイルだけにsanitizerを付ける構成とは区別しています。

追加した完全sanitizerビルドのcore全試験は、変更していない`src/core/notification.c:512`で停止しました。
空の通知本文に対し、NULLを`memchr()`へ渡す処理が報告されています。
この失敗を`stage6-sanitized-green.log`へ残し、段階9の全体検証で基準版との関係を確認します。
ファイル名にgreenを含みますが、終了コード1であり成功には数えていません。

## 段階7の行結果再利用

所有済みsnapshotへ論理行の先頭・末尾位置を追加し、UTF-8展開より前に行を判定します。
再利用のキーは画面、行ID、行revision、投影バイト数と列範囲です。
認識器の登録ごとに保持領域を分け、登録解除時に破棄します。
一致行は完成済みの現在結果からコピーし、キャプチャを参照保持します。
結果集合と本文のrevisionを更新し、`match_id`は維持します。
1回のbudgetでコピーする一致は1件までです。

一致0件の行には、結果管理とは別の2,048件までのcacheを設けました。
照会・保存はそれぞれ最大8項目を比較します。
衝突や容量不足は再走査になり、`INCOMPLETE`を発生させません。
64bit環境では1項目72バイト、認識器1個につき最大144 KiBで、登録数Nに対して最大N倍です。
最初の不一致行で必要になったときだけ確保し、確保できなければ通常の走査を続けます。
既存の一致管理の`dirty_line_capacity`は変えていません。

snapshotの行索引は論理行1本当たり16バイト、現在・作業中の結果検索索引は各保持行当たり8バイトです。
結果管理行の追加情報は各24バイト、一致結果の保持費用の記録は各8バイト増えます。
いずれも既存のsnapshot・行・結果の容量上限に従います。
未変更行の検索は索引上の二分探索で、比較量には索引の整列も含めて測定します。
整列には[標準qsortの契約](https://sourceware.org/glibc/manual/latest/html_node/Array-Sort-Function.html)とシステムヘッダーのコメントを確認しました。

一致行の行準備を更新行に限定する試験を追加し、従来実装でREDを確認しました。
初版では結果行そのものを整列して通知順序まで変わったため、行を挿入した後の通知順を検証する試験もREDになりました。
索引だけを整列する構成へ直し、文書上の順序を維持しています。
1,024行に既存一致がある状態で別の1行を更新すると、次のように変わりました。

| 指標 | 段階6 | 段階7 |
| --- | ---: | ---: |
| work | 68,661 | 2,099 |
| 再開回数・budget 4,096 | 17 | 1 |
| 確保回数 | 3,102 | 31 |
| 位置表の比較回数 | 45,416 | 38 |
| snapshot投影セル数 | 164,000 | 164,000 |

段階7でUTF-8を展開したのは変更行の3文字だけで、1,024行を再利用しました。
同じ規模の不一致行では1,022行を再利用し、cacheの衝突で2行を再走査しています。
展開した文字は11、位置表の比較は66回でした。
128行が収まる試験では変更行の3文字だけを展開します。
8,193行の履歴でも、結果容量・結果管理行容量を各1にしたまま末尾の一致へ到達します。
cacheの保持件数と比較量にも上限を設けて検証しました。

主画面・代替画面・BOTH、履歴削除、折り返し・resize、走査中の別行更新、イベントoverflowの既存試験が成功しています。
追加試験では、結合文字のキャプチャ範囲、名前付きキャプチャ、現在の本文revision、`match_id`、通知順序を確認しました。
認識器とterminalを破棄した後も、修正前後に保持したキャプチャの内容が同じです。
通常のcore全試験、文字処理self-test、例の文字方針とOSCの回帰試験、GTK3／GTK4の有限PTY認識が成功しました。
全objectを計測するsanitizerビルドでも、snapshot・行再利用・キャプチャ関連の試験は成功しています。
ログは`stage7-reuse-red.log`、`stage7-order-red.log`、`stage7-index-core.log`、`stage7-counters-self-test.log`、
`stage7-captures-core.log`、`stage7-focused-green.log`、`stage7-integration-green.log`です。

btopの41状態は全12試行で段階1と同じ本文hash・0一致・認識完了・イベント消費になりました。
workは9,007,713から3,167,755、再開回数は2,214から794へ減っています。
再利用した行は4,308、UTF-8展開は197,220文字でした。
core全体のCPU時間の中央値は、詳細計測で1.966〜1.977 msから1.317〜1.320 msへ減りました。
対照は1.834〜1.847 msから1.285〜1.340 msです。
対照の修正後2組目だけCPU周波数の中央値が5.113 GHzで、他は5.167〜5.200 GHzでした。
この低い群を改善率へまとめず、試行ごとに記録しています。
分布、コマンド、hashは[段階7の比較JSON](plan4-measurements/step7-line-reuse-comparison.json)にあります。

認識区間のCPU時間は約1.27 msから約0.63 msへ減りましたが、snapshot作成は約0.27〜0.28 ms残っています。
変更が1行だけの条件でも全164,000セルを投影しており、再開budgetの外で行う仕事は減っていません。
この負荷が主要部分として残るため、段階8の変更行投影と分割構築へ進みます。

## 段階8. 変更行の投影と再開予算

最初に、認識を再開するたびの本文同期を除きました。
入力・入力終了・resize・履歴容量変更は、各公開操作が戻るまでに本文メタデータを同期します。
budget 1で長い折り返し行を再開する試験では、修正前は1,025断片を同期し直してREDになりました。
修正後は64回の再開すべてで同期断片数が0となり、core全試験と文字処理self-testも成功しています。
ログは`stage8-sync-red.log`、`stage8-sync-green.log`、`stage8-sync-core-green.log`です。
本文を変更する操作自体の同期量は、別の指標として引き続き記録します。

### 変更行だけの所有済み投影

認識では、論理行のID・revision・投影バイト数・列範囲から、既存の一致・不一致結果を先に調べます。
結果を再利用できる行はセルを投影しません。
変更行だけ、所有するUTF-8本文と位置表を段階的に作ります。公開snapshot APIの契約は変えていません。
段階7で加えた全体snapshotの行索引と内部照会関数は不要になったため、削除しました。

論理行の列挙は1 workにつき1物理断片です。
投影は1 workにつき最大1セルを読み、最大4バイトをコピーするか、境界・空白を追加します。
通常の再開では、メタデータ訪問も使用work以内です。
長い1論理行でもセルへの借用ポインターを持ち越さず、再開時に行のID・revision・列範囲を検証します。
1,048,576文字の行を走査する試験とbudget 1の試験で、実際のセル訪問・バイトコピーを計数して上限を確認しました。

途中・完成済みの投影は認識器ごとに最大64行、合計本文バイト数は`snapshot_byte_capacity`以下です。
容量不足では古い投影を捨てて作り直し、認識結果の上限を変更しません。
別行の更新で全体走査が再開しても、同じ行の途中投影を保持します。
行挿入によって物理的な位置が変わり、さらに前方の別行を4回書き換える試験でも、
128文字の対象行を投影したのは1回だけでした。
対象行自身の追記・連続書き換え、画面切り替え、resize、履歴削除の後も、入力が止まればbudget 1で最新結果まで完了します。

64bit環境では物理断片のメタデータが8バイト増えます。
投影の保持件数をE、合計本文バイト数をBとすると、保持領域の上限はおおむね
`144 × 64 + 336 × E + 56 × (2 × B + 2 × E) + B + E`バイトです。
これは行の索引、投影状態、位置表、本文、空本文用の最小確保を含み、allocatorの管理領域を除きます。
一致結果、UTF-8展開領域、段階7の不一致cacheは別で、各既存容量制約に従います。
全体snapshotの各行16バイトの索引は、この段階では不要です。

### 回帰試験で確認した境界

1行更新の試験は、修正前の5,280セル投影でREDとなり、修正後は6セルになりました。
Unicode・結合文字・空白・空行・折り返しについて、分割投影の本文と全バイト境界を公開snapshotと比較しています。
terminal破棄後にも所有データを照会して一致することを確認しました。
主画面・代替画面・BOTHの本文・行容量は、登録時の拒否と更新後の`SCOPE_INCOMPLETE`の両方を検証しています。

この検証で、代替画面を狭くした後に古い投影長が残る問題を検出しました。
コピー・再配置した行をresize時に再同期し、切り捨てと幅の広い文字の補修を反映します。
同じ本文を別の列へ移した場合も位置表が変わるため、公開の本文revisionを維持したまま内部の位置更新を通知します。
両ケースをREDからGREENにしました。
投影の3か所の確保をそれぞれ失敗させる試験では、途中で失敗しても行を飛ばさず、容量を二重計上せずに再試行します。

通常のcore全試験、文字処理self-test、GTK3／GTK4の有限PTY認識、rendering計測の回帰試験が成功しました。
snapshot・認識・キャプチャ・今回の境界試験は、全objectにASan／UBSanを適用したビルドでも成功しています。
ログは`stage8-projection-red.log`、`stage8-position-red.log`、`stage8-retry-red.log`、`stage8-column-red.log`、
`stage8-column-green.log`、`stage8-final-workload.log`、`stage8-final-focused-green.log`、`stage8-final-integration-green.log`です。

### 計測結果と予算外に残る処理

1,024行の既存一致があり、別の1行を更新する条件で比較しました。

| 指標 | 段階7 | 段階8 |
| --- | ---: | ---: |
| 投影セル数 | 164,000 | 6 |
| 投影バイト数 | 全履歴の測定・書き込み | 3 |
| work | 2,099 | 2,111 |
| 再開回数・budget 4,096 | 1 | 1 |
| 位置表の比較 | 38 | 17 |
| 確保回数 | 31 | 31 |

投影の仕事をbudgetに加えたため、workが少し増えています。
btopの41状態でもworkは3,167,755から3,956,709、再開は794から991へ増えました。
一方、実際の投影セルは2,056,560から394,440、投影バイトは1,460,574から281,981へ減りました。
全12試行で段階1と同じ41個の本文hash・0一致・認識完了・イベント消費を確認しています。

詳細計測のCPU時間中央値は、snapshot区間が0.275〜0.282 msから0.088〜0.090 ms、
認識区間が0.627〜0.637 msから0.431〜0.439 msになりました。
core全体は詳細計測で1.321〜1.329 msから1.107〜1.111 ms、対照で1.280〜1.405 msから1.127〜1.148 msです。
試行ごとのCPU周波数中央値は5.157〜5.200 GHzで、周波数と分布は試行ごとに残しています。
詳細計測では新しい分割投影の作成・再開にも時計を入れ、旧APIを呼ばなくなっただけで時間が0になる計測を避けました。
[段階8の比較JSON](plan4-measurements/step8-bounded-projection-comparison.json)に、実行ファイルのhash、各分布、仕事量、容量の計測値があります。

公開の再開API全体がbudgetだけで時間上限を持つ、という意味ではありません。
登録・明示的な再走査はbudgetを受け取らず、同期済みメタデータで全文の容量を確認します。
登録時の既存の本文同期も残っています。通常の本文変更に伴う同期は、入力側の処理です。
btopの41状態では、同期が10,824断片・514,900セル・716,004バイトを訪問しました。
認識の断片列挙・行検証は6,737回、投影cacheの比較は75,072回でした。
cache検索は最大64項目で、容量を空ける際も最大64件の破棄と各64項目の比較に限られます。

結果集合の確定は既存の一括処理を保ちました。
1,024一致行の試験では、確定の行・結果訪問が5,123回、旧結果の解放が1,024件、
結果行索引の整列・検索などの比較が32,848回です。btopは0一致なので、確定の結果訪問は0でした。
途中更新で破棄する作業中の結果、配列の拡張、キャプチャの所有コピーも容量に依存する予算外の処理です。
これらは既存の結果数・行数・バイト数上限に従います。
今回は投影の訪問量を削減・分割でき、結果確定がbtopの主な負荷ではないため、一括確定の置き換えは行いませんでした。

## 残る作業

段階1〜8の変更対象の完了条件を確認しました。段階9は未完了です。
全体の統合検証を行います。
全体テスト`make -j1 test`、実アプリでの比較、動画・実機確認も最後に必要です。

計測で使うGLib sourceの優先度と寿命は[公式main loop文書](https://docs.gtk.org/glib/main-loop.html)と[2.80.0のAPIコメント](https://github.com/GNOME/glib/blob/2.80.0/glib/gmain.c)で確認しました。
画素の取得は[GTKの公開API](https://docs.gtk.org/gdk3/func.pixbuf_get_from_window.html)と[3.24.41のAPIコメント](https://github.com/GNOME/gtk/blob/3.24.41/gdk/gdkpixbuf-drawable.c)に従っています。
