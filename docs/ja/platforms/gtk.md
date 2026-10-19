# GTK 3／4へ組み込む

[English](../../en/platforms/gtk.md)

boxrig UIのGTK版を使うと、GTKアプリケーションへ端末ウィジェットを配置できます。
端末の文字や画像を描画し、選択、コピー、IME入力をGTKの画面に組み込めます。
端末の内容はboxrig coreが管理し、GTKがウィジェットの配置とネイティブ入力を扱います。
アプリケーションは通信、モデルの同期、フレーム処理を接続します。

## GTKの版と必要なライブラリ

GTK 3は3.24以降、GTK 4は4.14以降をビルド条件としています。
別のライブラリ、別の型であり、一つのアプリケーションには使用中のGTKと同じ版をリンクしてください。

| GTK | SDKのインストール | pkg-config | 公開ヘッダー |
| --- | --- | --- | --- |
| 3 | `make install-gtk3 PREFIX=...` | `boxrig-gtk3` | [gtk3.h](../../../include/boxrig/gtk3.h) |
| 4 | `make install-gtk4 PREFIX=...` | `boxrig-gtk4` | [gtk4.h](../../../include/boxrig/gtk4.h) |

依存の準備と`PREFIX`の使い方は[ビルドガイド](../building.md)を参照してください。

## ウィジェットを配置する

以下はGTK 4で、既存のコンテナーへ追加する端末ウィジェットを作る関数です。
GTKと端末、UIモデルを初期化してから呼び出す生成部分の抜粋です。
ウィジェットを配置し、この後のフレーム処理を接続すると、端末へ渡した文字や画像を表示できます。

```c
#include <boxrig/gtk4.h>

boxrig_ui_status create_terminal_widget(boxrig_ui_model *model,
    BoxrigGtk4TerminalWidget **widget_out)
{
    boxrig_gtk4_terminal_widget_config config =
        boxrig_gtk4_terminal_widget_config_default();
    return boxrig_gtk4_terminal_widget_create(model, &config, widget_out);
}
```

返るのはfloating referenceです。
使用中のGTKのコンテナーへ配置し、参照を管理します。
GTK 3ではヘッダー、型名、関数名の`gtk4`／`Gtk4`を対応するGTK 3のものへ置き換えます。
端末とモデルはウィジェットより長く保持してください。
GTK自身の配置と寿命の規則は[GtkWidgetの説明](https://docs.gtk.org/gtk4/class.Widget.html)に従います。

実際の配置は[GTK 3アプリ](../../../examples/boxrig_term/gtk3/application.c)と[GTK 4アプリ](../../../examples/boxrig_term/gtk4/application.c)で確認できます。

作成時からフォントを指定するには、設定の`initial_font`へ`boxrig_font_specification`を渡してください。
`NULL`なら既定のフォントを使います。フォント名などのデータは作成時にコピーされます。
既存の表示先とモデルを共有する場合、同じセル寸法になるフォントを指定してください。
例えば、停止したウィジェットを置き換えるときは、古いウィジェットの`get_font_specification()`で取得した指定を渡せます。
古いウィジェットを保持したまま生成でき、初期フォントの寸法が一致しないために生成が失敗する状況を避けられます。
ファイルやメモリーから登録したフォントのリソースIDはウィジェット固有であり、新しいウィジェットへ流用できません。

## フレーム要求と通信を接続する

設定の`frame_request_callback`は、アプリケーションのメインループへ処理を予約するための通知です。
コールバック内から再帰的に描画せず、同じ要求をまとめて後で処理します。

その処理ではセッションを進め、`boxrig_ui_model_synchronize()`、`boxrig_gtk4_terminal_widget_frame()`の順に呼びます。
GTKがdrawやsnapshotを呼ぶだけでは、coreの変更はモデルへ同期されません。
`frame()`は待機せず、残作業とアニメーションの予定を返します。

PTYの準備状態をGTKのメインループへ接続する具体例は、[GTK 4のイベントソース](../../../examples/boxrig_term/gtk4/event_source.c)を参照してください。

## サイズとスクロールを扱う

GTKから割り当てられた領域に従ってレイアウトが決まります。
希望する行列数は`set_preferred_grid()`で指定し、実際の配置結果は`get_layout()`で取得します。
行列数の変更は通信先へ反映してから`complete_grid_resize()`で完了を返してください。

スクロール位置はウィジェットが提供するadjustmentと共通のスクロールバーモデルから扱えます。
アプリ独自のバーを使う場合は[GTK 3の例](../../../examples/gtk3/external_scrollbar.h)または[GTK 4の例](../../../examples/gtk4/external_scrollbar.h)を参照してください。
これらはアプリへ取り込むヘッダー例であり、単独実行するプログラムではありません。

## CairoとOpenGLを選ぶ

既定の`render_policy`はCairoです。
OpenGLを希望する設定と、OpenGLを必須にする設定があります。
後者は必要な描画環境が得られなければ失敗します。
任意のOpenGL対応は`GTK_OPENGL=0`でビルドから除外できます。

生成成功だけでは描画方式の初期化完了を意味しません。
ウィジェットがrealizeされ、最初のフレームが完成した後に`get_render_state()`で実際の方式を確認します。
切り替え中の`pending`が解消するまで、フレームとGTKイベントを処理してください。
失敗した切り替えでは、利用可能な以前の描画方式を保持する場合があります。

## 支援技術から本文を取得する

GTK 3のウィジェットは`AtkText`、GTK 4のウィジェットは`GtkAccessibleText`を実装しています。
本文の公開は既定で無効です。読み上げソフトや本文取得を使う自動テストでは、生成時の設定で`accessibility_enabled = true`を指定してください。
実行中は`set_accessibility_enabled()`で切り替え、`get_accessibility_enabled()`で設定を取得できます。
無効にすると、公開済みの本文を取り下げ、本文データの生成と変更通知を停止します。再び有効にすると、次の`frame()`で現在の表示範囲を公開します。同期出力中は、その出力が完了するまで公開を待ちます。
メニューやボタンなど、GTK標準の支援機能には影響しません。
有効な場合も、本文・文字属性・配置・選択範囲・カーソル位置に変更がなければ、公開済みの本文データを再利用します。点滅やIMEの未確定文字表示だけでは本文を再生成しません。

AT-SPI経由で、表示中の本文、端末カーソルに対応するcaret、表示ごとの選択範囲と文字属性を取得できます。
本文の位置はUnicodeスカラー単位です。全角文字の継続セルは文字数へ加算せず、結合文字はそれぞれのスカラーを数えます。
GTK 3で取得できる文字の表示領域は、書記素が占めるセルに対応します。

本文には表示領域内の行を含め、履歴全体は含めません。
履歴移動、代替画面への切り替え、幅変更による折り返し直しに追従します。
自動折り返しでは改行を加えず、実改行と空行にはLFを返します。
未使用の行末セルは省略しますが、明示的に出力された空白と行頭の空白は保持します。
IMEの未確定文字は本文に含めません。

アプリケーションが`frame()`を処理すると、本文、caret、選択の変更を通知します。
同期出力中は直前の本文を維持し、点滅だけでは本文を再通知しません。
削除通知中は旧本文、挿入通知中は新本文を参照できます。
caretが表示範囲外にある場合は本文末尾の位置を返します。
支援技術からのcaret移動と選択の変更は受け付けず、子プロセスへ入力を生成しません。

1回に公開する本文と位置情報の保存量は16MiBが上限です。
上限を超える場合、`frame()`は`BOXRIG_UI_LIMIT_EXCEEDED`を返し、直前の本文を保持します。
取得APIの契約は[AtkText](https://docs.gtk.org/atk/iface.Text.html)と[GtkAccessibleText](https://docs.gtk.org/gtk4/iface.AccessibleText.html)を参照してください。

GTK 4は最低版4.14の本文・caret・選択・文字属性と変更通知に対応しています。
4.16以降の座標取得と、4.22以降のcaret・選択変更は提供しません。
GTK 4.14でもAT-SPI経由の本文取得とOrcaの読み上げ文字列生成を確認しています。
文字属性だけが変わった場合は、新しい属性を取得できますが、GTK 4.14には属性専用の公開通知APIがありません。
利用できる操作と通知は[GtkAccessibleTextの版別API](https://docs.gtk.org/gtk4/iface.AccessibleText.html)に従います。

## 入力と破棄の注意点

IMEの選択は描画方式とは独立しています。
`ime_policy`と`candidate_policy`で希望を指定し、`get_ime_state()`で実際の入力経路と候補表示を確認します。
通常の端末の入力と、アプリが所有する編集文書への入力は別の接続です。

一時的にユーザー入力を禁止する場合は、`set_input_enabled(widget, false)`を使います。
対象ウィジェットのキー・IME確定・貼り付け・端末向けマウス報告を止めますが、選択・コピー・履歴移動・描画と端末の応答は継続します。
再開の指定は同じ関数の`true`です。禁止前のIME・clipboard応答は再開後に破棄され、禁止をまたいで押されているキーは離すまで送信されません。

開始済みpasteの終端を送信バッファへ追加できない場合、禁止要求は受け付けたうえで`BUSY`を返します。
`get_input_state()`が返す`STOPPING`の間は通常のフレーム処理を続け、`DISABLED`になってから再開してください。
入力処理中のコールバックから要求した場合も`BUSY`ですが、この場合は状態を変更しません。
`DISABLED`は終端をcoreへ投入した状態で、通信先への送信完了ではありません。
すでにcoreへ渡したデータや、同じ端末を使う別ウィジェットの入力は、このAPIの対象外です。

破棄時はフレーム予約や通信側の参照を解除し、`quiesce()`で新しい処理を止めてからウィジェットを破棄します。
モデルと端末を先に解放しないでください。
`quiesce()`は取り消せません。

[GTK 3のAPI](../../../include/boxrig/gtk3/terminal_widget.h)、[GTK 4のAPI](../../../include/boxrig/gtk4/terminal_widget.h)、[文書目次](../index.md)
