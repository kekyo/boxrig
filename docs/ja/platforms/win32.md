# Win32アプリケーションへ組み込む

[English](../../en/platforms/win32.md)

boxrig UIのWin32コンポーネントを使うと、既存の`HWND`内へ端末の子ウィンドウを配置できます。
boxrig coreで解釈した文字や画像を表示し、キーやマウス、IMEから操作できます。
標準描画、OpenGL、Direct3D 11で共通の入力・配置・フレーム操作を使えます。

親ウィンドウとメッセージループはアプリケーションが用意します。
シェルを動かす場合はPTYを接続しますが、固定文字列や独自の通信路だけでも利用できます。

## SDKと対象アーキテクチャ

SDKはamd64とi686を別々に生成します。
標準版は`make install-win32-amd64 PREFIX=...`、32ビット版は末尾を`i686`にしたターゲットを使います。
リンクするアプリとSDKのアーキテクチャをそろえてください。

core、UI、PTYではOS要件が異なります。
UIが動くことはOS付属ConPTYが使えることを意味しません。
SDKの生成とリンクは[ビルドガイド](../building.md)、疑似コンソールの接続は[セッション](../sessions.md)を参照してください。

## 親ウィンドウへ配置する

以下は、既存の親ウィンドウ内に端末の子ウィンドウを作る関数の例です。
同じUIスレッドで作成した親ウィンドウと、端末を参照するUIモデルを渡します。
生成部分の抜粋なので、配置とメッセージ・フレーム処理はこの後の説明に従って接続してください。

```c
#include <boxrig/win32.h>

boxrig_ui_status create_terminal_child(HINSTANCE instance, HWND parent,
    boxrig_ui_model *model, boxrig_win32_terminal_component **component_out)
{
    boxrig_win32_terminal_component_config config =
        boxrig_win32_terminal_component_config_default();
    return boxrig_win32_terminal_component_create_child(
        instance, parent, model, &config, component_out);
}
```

生成後に領域を割り当て、表示を有効にします。
コンポーネントは子ウィンドウと表示先を所有し、親とモデルは借用します。
アプリ所有の描画先へ接続する`create_on_target()`では、メッセージ転送と描画先の寿命もアプリ側の責任になります。

## メッセージ処理と描画

メッセージループから`pretranslate_message()`を呼び、処理済みかどうかを確認します。
アプリが直接転送した入力の結果が`BLOCKED`なら、その入力を保持して容量回復後に再試行します。
親や兄弟ウィンドウのメッセージは通常どおりアプリで処理してください。

セッションの処理、モデルの同期、コンポーネントの`frame()`を順に進めます。
`additional_dispatch`がなければ、ネイティブメッセージ、PTYのactivity、必要な描画完了をまとめて待ちます。

描画方式によっては`activity_handle()`がハンドルを返します。
待機でそのハンドルの通知を受け取ったときだけ`dispatch_activity()`を呼びます。
通知を待たずに呼んで描画を進める用途ではありません。
全体の接続は[コンポーネント例](../../../examples/win32/component.c)を参照してください。

## DPIとリサイズ

実際のクライアント領域を`allocate()`へ渡し、確定した行列数を端末とPTYへ反映します。
DPIやフォント変更によりセル寸法も変わるため、古い寸法から列数を計算し続けないでください。
モデルに対する行列数の決定役は一つです。

親が画面上を移動した場合は、候補ウィンドウの位置にも反映します。
`resynchronize_ime_placement()`は移動後のIME位置を再通知するAPIです。
これ自体は描画やモデル同期を行いません。

## 描画方式を選ぶ

| 用途 | API・ガイド |
| --- | --- |
| 標準のWin32表示 | このページの`create_child()` |
| OpenGLの子ウィンドウ | [OpenGLコンポーネント](../../../include/boxrig/win32/opengl_component.h)と[描画方式](../renderers.md) |
| Direct3D 11の子ウィンドウ | [Direct3D 11コンポーネント](../d3d11-component.md) |
| アプリ所有のDirect3D描画先 | [Direct3D 11レンダラー](../d3d11-renderer.md) |

方式を切り替えても端末とモデルは保持できます。
旧コンポーネントが持つレイアウトの決定権と描画資源を解放してから、新しいコンポーネントを接続します。

## 透過と親ウィンドウの条件

端末の背景にalphaを付ける設定と、トップレベルをデスクトップへ透過する設定は別です。
描画方式に合う親ウィンドウの構成が必要で、コンポーネントが親のスタイルを変更するわけではありません。

標準描画の例は[host_opacity.c](../../../examples/win32/host_opacity.c)、Direct3D 11の例は[専用ガイド](../d3d11-component.md#透過とサイズ変更)にあります。
設定した値だけで成功と判断せず、実際の透過状態を取得してください。

## 解放と配布先の確認

作成したUIスレッドで、待機登録と入力を止めてからコンポーネントを破棄します。
その後にモデル、端末、親の資源を解放します。
通信セッションがある場合は、それも端末とPTYより先に停止してください。

配布先では、描画API、GPU、ConPTYの可否を分けて確認します。
インストールされる例と公開APIの詳細は[Win32ヘッダー](../../../include/boxrig/win32.h)と[SDKのビルド](../building.md)からたどれます。

[文書目次](../index.md)
