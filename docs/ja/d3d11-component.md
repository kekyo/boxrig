# Direct3D 11の子ウィンドウを使う

[English](../en/d3d11-component.md)

boxrig UIのDirect3D 11コンポーネントを使うと、既存のWin32ウィンドウ内へGPUで描く端末を配置できます。
端末、モデル、入力、配置のAPIは標準Win32コンポーネントと共通です。
アプリケーションが親ウィンドウを用意し、コンポーネントがデバイス、swapchain、表示の完了待機を管理します。

[実行例](#実行例)では、固定文字列の表示、透過、描画方式の切り替えを試せます。
すでにアプリケーションが持つtextureへ描く場合は、[Direct3D 11レンダラー](./d3d11-renderer.md)を使ってください。

## 使える環境とSDK

Direct3D 11とDirect2D 1.1が必要です。
APIの提供条件にはWindows 7 SP1のPlatform UpdateまたはWindows 8以降が含まれますが、実際に必要なinterfaceと描画先を生成できることも条件です。
透過の合成には追加の機能が必要です。
[MicrosoftのPlatform Updateの説明](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/platform-update-for-windows-7)を参照してください。
OSとGPUに依存する表示・透過の確認方法は、[実機のテスト手順](./development/testing.md#windows実機を準備する)を参照してください。

<a id="配布sdkと実行可能な例"></a>

リポジトリのルートで、アーキテクチャごとに別のSDKを作ります。

```sh
make install-win32-d3d11-amd64 PREFIX="$PWD/build/sdk-d3d11-amd64"
export PKG_CONFIG_LIBDIR="$PWD/build/sdk-d3d11-amd64/lib/pkgconfig"
x86_64-w64-mingw32-gcc -std=c99 -Wall -Wextra -Wpedantic -Werror \
  build/sdk-d3d11-amd64/share/boxrig/examples/win32/d3d11_component.c \
  $(pkg-config --cflags --libs --static boxrig-win32-d3d11) \
  -o build/d3d11_component.exe
```

i686ではターゲット、prefix、コンパイラーをi686用へ変更します。
このモジュールは静的ライブラリです。
boxrigのDLLを追加配置する必要はありません。

## 親ウィンドウへ配置する

以下は、GPUで描画する端末の子ウィンドウを作る関数です。
COM、親ウィンドウ、端末とUIモデルを用意した後に呼ぶ生成部分の抜粋です。
配置とフレーム処理を接続すると、coreへ渡した文字を表示できます。
生成失敗の詳しい理由は`native_result`へ返ります。

```c
#include <boxrig/win32/d3d11_component.h>

boxrig_ui_status create_d3d11_child(HINSTANCE instance, HWND parent,
    boxrig_ui_model *model, HRESULT *native_result,
    boxrig_win32_terminal_component **component_out)
{
    boxrig_win32_terminal_component_config config =
        boxrig_win32_terminal_component_config_default();
    boxrig_win32_d3d11_config graphics = boxrig_win32_d3d11_config_default();
    graphics.creation_result = native_result;
    return boxrig_win32_d3d11_terminal_component_create_child(
        instance, parent, model, &config, &graphics, component_out);
}
```

既定ではハードウェアのデバイスを要求します。
失敗してもWARPや標準描画を自動で選び直しません。
生成時の出力ポインターは生成中だけ使用され、コンポーネントに保持されません。
全体の成否は`boxrig_ui_status`で判断します。

## 描画と待機を進める

モデルの同期後に通常の`boxrig_win32_terminal_component_frame()`を呼びます。
`additional_dispatch`が偽になったら、ネイティブメッセージと`activity_handle()`を合わせて待機します。
そのハンドルの通知を実際に受け取ってから`dispatch_activity()`を呼び、次のフレームを進めます。

`frame_pending`が真という理由だけでループを回し続けないでください。
生成、入力、描画、破棄は作成スレッドで行います。
全体例の[run_component](../../examples/win32/d3d11_component.c)で待機との接続を確認できます。

## 透過とサイズ変更

背景のalphaとトップレベルの透過は区別します。
コンポーネントが提供する表示面のalphaだけでは、デスクトップまで透けるとは限りません。
親を管理するアプリが合成条件を用意します。

付属の`--transparent`例は、非layeredの親に`WS_EX_NOREDIRECTIONBITMAP`を使う構成です。
親のスタイルの意味は[Windowsの拡張ウィンドウスタイル](https://learn.microsoft.com/en-us/windows/win32/winmsg/extended-window-styles)を参照してください。
`--fallback`では、コンポーネントと親ウィンドウを破棄し、標準描画用の親を作り直します。
透過用の親のスタイルを、そのまま不透明描画へ引き継がない構成です。

通常の親を維持しながら背景透過と全体半透明を切り替える構成は、付属端末の[透過制御](../../examples/boxrig_term/win32/opacity.c)を参照してください。
親の背景描画や属性の適用は親の所有者が行います。

サイズ変更は共通の`allocate()`へ渡します。
新しい描画先の準備中も、直前に完成した表示を保持します。
ネイティブの寸法と端末の行列数を確定してから、それぞれの完了処理を返してください。

## 障害を調べて復旧する

`get_diagnostics()`にはHRESULT、合成の失敗、デバイスと描画先の世代、実際のalpha、受理したフレーム数が残ります。
DirectCompositionだけが使えない場合は、不透明描画を利用できることがあります。
必須の透過要求は満たせなければ失敗します。

管理対象のデバイス喪失はコンポーネントが資源を再構成します。
復旧不能なら、アプリケーションがコンポーネントと必要な親を破棄し、同じ端末とモデルで別の方式を生成します。
切り替えを選ぶ処理はアプリ側へ明示的に置きます。

## 実行例

Windowsで`d3d11_component.exe`を実行すると固定文字列の端末を表示します。
`--check`は最初のフレームを完了して終了します。
`--transparent --fallback`は透過表示から標準描画への作り直し、`--warp`は明示したWARPでの動作確認です。

[完全なソース](../../examples/win32/d3d11_component.c)、[Win32の組み込み](./platforms/win32.md)、[文書目次](./index.md)
