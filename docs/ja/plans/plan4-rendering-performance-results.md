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
現在は専用targetの`make test-gtk-sample-recognition`で実行し、修正後に全体テストへ登録します。

試験作成中には、GTK4の初期allocationが完了する前に測定を始める失敗もありました。
それは認識停止の証拠に含めず、表示とlayoutの初期化完了を待つように試験を修正しました。
詳細は`build/test-results/plan4/sample-gtk3-red.log`と`sample-gtk4-red.log`にあります。

## 残る作業

段階2〜9は未完了です。
次に認識・イベント処理の未完了状態を描画要求から分離します。
RGB表示、CPU描画の削減、認識の順次走査と行再利用は、計画の順に実施します。
snapshotの分割は、段階7後の負荷を測って着手を判断します。
全体テスト`make -j1 test`、実アプリでの比較、動画・実機確認も最後に必要です。

計測で使うGLib sourceの優先度と寿命は[公式main loop文書](https://docs.gtk.org/glib/main-loop.html)と[2.80.0のAPIコメント](https://github.com/GNOME/glib/blob/2.80.0/glib/gmain.c)で確認しました。
画素の取得は[GTKの公開API](https://docs.gtk.org/gdk3/func.pixbuf_get_from_window.html)と[3.24.41のAPIコメント](https://github.com/GNOME/gtk/blob/3.24.41/gdk/gdkpixbuf-drawable.c)に従っています。
