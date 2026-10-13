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

## 入力と破棄の注意点

IMEの選択は描画方式とは独立しています。
`ime_policy`と`candidate_policy`で希望を指定し、`get_ime_state()`で実際の入力経路と候補表示を確認します。
通常の端末の入力と、アプリが所有する編集文書への入力は別の接続です。

破棄時はフレーム予約や通信側の参照を解除し、`quiesce()`で新しい処理を止めてからウィジェットを破棄します。
モデルと端末を先に解放しないでください。
`quiesce()`は取り消せません。

[GTK 3のAPI](../../../include/boxrig/gtk3/terminal_widget.h)、[GTK 4のAPI](../../../include/boxrig/gtk4/terminal_widget.h)、[文書目次](../index.md)
