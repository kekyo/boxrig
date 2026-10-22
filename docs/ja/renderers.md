# 描画方式を選ぶ

[English](../en/renderers.md)

boxrig UIは、既存のウィンドウ内へ端末を表示する構成と、アプリケーションのGPU描画先へ端末を描く構成の両方に使えます。
ウィンドウへの配置には環境別コンポーネント、アプリケーションのFBOやtextureへの出力にはレンダラーを選びます。
端末の文字や属性を保持するboxrig coreは、どちらの構成でも共通です。

描画を全て自分で実装する場合は、coreからセルを取得する方法もあります。
その最小例は[README](../../README_ja.md#画面を使わずに端末出力を解釈する)、実装手順は[core向けstep-by-step](./step-by-step/step-by-step-core.md)を参照してください。

## 標準コンポーネントと描画API

コンポーネントを使うと、入力、配置、フォント、描画先、表示完了をまとめて扱えます。
まず[X11](./platforms/x11.md)、[GTK](./platforms/gtk.md)、[Wayland](./platforms/wayland.md)、[Win32](./platforms/win32.md)から環境を選んでください。

レンダラーだけを使う場合は、表示先の生成、割り当て、フレームの受理、ネイティブな表示、資源の寿命をアプリケーションが管理します。
受信データとモデルの同期は、どちらでも共通です。

## 描画方式を選ぶ

| 方式 | 主な用途 | アプリケーションが用意するもの |
| --- | --- | --- |
| Pango/Cairo | Linuxの標準コンポーネント | 親ウィンドウ、GTKコンテナー、親surfaceなど |
| DirectWrite/GDI | Win32の標準コンポーネント | 親ウィンドウ |
| OpenGLコンポーネント | X11、Wayland、Win32のGPU表示 | 対応する親とイベントループ |
| GTKのOpenGL設定 | GTKウィジェット内のGPU表示 | GTKの配置とフレーム接続 |
| OpenGLレンダラー | アプリのFBOなどへ描画 | context、関数解決、描画先とその世代 |
| Direct3D 11コンポーネント | Win32のGPU表示と透過 | 条件に合う親ウィンドウ |
| Direct3D 11レンダラー | アプリのtextureへ描画 | deviceと描画先 |

どの方式でも、端末の文字や属性はcoreに保持されます。
描画方式の選択は、端末プロトコルを変更する操作ではありません。
任意の描画ライブラリをリンクしていない構成でも、標準のコンポーネントを利用できます。

## OpenGLの描画先を用意する

[OpenGLの公開API](../../include/boxrig/render/opengl.h)は、OpenGL 3.3 coreまたはOpenGL ES 3.0を対象とします。
アプリはcurrentなcontext、関数解決コールバック、contextとスレッドの識別情報を渡します。
ライブラリがcontextを作ったり、勝手にcurrentへ切り替えたりはしません。

描画先にはRGBA8、sRGB8 alpha8、不透明なRGB8を使えます。
alphaの解釈、物理ピクセルの寸法、単調増加する世代を明示します。
RGB8にはalphaチャンネルがないため、`BOXRIG_GL_ALPHA_OPAQUE`を指定してください。
RGB8はalphaがないことを検証できる、名前が0以外のフレームバッファーで使えます。
現在の描画先の契約はマルチサンプルを受け付けません。
詳しい条件は[runtime.h](../../include/boxrig/render/runtime.h)にあります。

まずは[実行可能なFBOの例](../../examples/opengl_renderer/main.c)を使うと、contextの生成から画素の確認まで追えます。
SDKは`make install-render-ogl PREFIX=...`、pkg-config名は`boxrig-render-ogl`です。

## 描画完了と再描画

以下は、端末をアプリケーションのFBOへ描き、その描画結果を受理する関数です。
レンダラーとそれに対応するlifecycle、配置・同期済みのUI表示先を用意してから呼びます。
ウィンドウへの表示は含めず、FBOの画素を採用した時点で完了とする抜粋です。

```c
#include <boxrig/ui/renderer_lifecycle.h>

boxrig_renderer_status draw_frame(boxrig_renderer_lifecycle *lifecycle,
    boxrig_ui_view *view, uint64_t target_generation)
{
    boxrig_renderer_frame_token token;
    boxrig_renderer_status status = boxrig_renderer_lifecycle_prepare_frame(
        lifecycle, view, NULL, target_generation, &token);
    if (status != BOXRIG_RENDERER_OK) return status;

    /* Accept the off-screen result without presenting a window. */
    return boxrig_renderer_lifecycle_accept_frame(lifecycle, &token);
}
```

ウィンドウへ表示するアプリでは、prepareの後で表示処理の成否を判断し、成功時にaccept、取り消す場合にabortを呼びます。
acceptするまで再描画が必要な範囲は保持されます。
prepareからacceptまたはabortまで、表示先と描画資源を維持してください。

レンダラー単体はswap、画素の読み戻し、完了までのブロッキング待機を行いません。
表示と待機をコンポーネントに任せる構成では、そのコンポーネントのactivityやイベントの契約に従います。

## 描画先の再生成

サイズが変わった場合は新しい描画先を用意し、新しい世代とともに渡します。
古い世代のフレームを新しい描画先へ受理しないでください。
変更に失敗したときの状態は戻り値と診断で確認します。

context自体が失われた場合は、古い識別情報を使って描画を続けず、資源とlifecycleを再構成します。
モデルと端末は描画先とは別に保持できるため、通信内容を失わず表示部分を作り直せます。

## Direct3D 11と独自実装への案内

[Direct3D 11コンポーネント](./d3d11-component.md)はウィンドウ表示までを、[Direct3D 11レンダラー](./d3d11-renderer.md)はアプリ所有の描画先への出力を扱います。

独自の描画方式を実装する場合は、[表示先](../../include/boxrig/ui/view.h)と[lifecycle](../../include/boxrig/ui/renderer_lifecycle.h)の公開契約を確認してください。
端末状態を読むだけでなく、再描画の範囲、文字配置、選択やIMEの重ね描き、フレームの受理を扱う必要があります。

[文書目次](./index.md)
