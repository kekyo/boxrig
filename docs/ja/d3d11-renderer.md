# アプリ所有のDirect3D 11描画先へ出力する

[English](../en/d3d11-renderer.md)

boxrig UIで配置した文字や画像を、アプリケーションが所有するDirect3D 11のtextureへ描けます。
別の描画と端末を合成する場合や、画面外で端末の画像を作る場合に使います。
[全体例](../../examples/win32/d3d11_renderer.c)では、描いた画素を読み戻して`boxrig-d3d11.ppm`へ保存できます。

deviceとtexture、画面へ表示する場合のswapchainとPresentは、アプリケーションが管理します。
端末の内容はboxrig core、文字の配置と描画はboxrig UIとこのレンダラーが担当します。

## コンポーネントとの使い分け

親ウィンドウへ配置して入力も受け取りたい場合は、[Direct3D 11コンポーネント](./d3d11-component.md)を使ってください。
レンダラー単体では、表示先の生成、レイアウト、入力、モデル同期をアプリ側で接続します。

SDKは`make install-render-d3d11-amd64 PREFIX=...`またはi686用のターゲットで作成します。
pkg-config名は`boxrig-render-d3d11`です。
インストール先の`share/boxrig/examples/win32/d3d11_renderer.c`が全体例です。

## デバイスと描画先を渡す

設定の`device`と`target`を埋めて`boxrig_d3d11_renderer_create()`へ渡します。
deviceはBGRAの相互運用が可能であること、描画先は同じdeviceのresourceであることが必要です。
[Direct2Dのsurface条件](https://learn.microsoft.com/en-us/windows/win32/api/d2d1_1/nf-d2d1_1-id2d1devicecontext-createbitmapfromdxgisurface%28idxgisurface_constd2d1_bitmap_properties1_id2d1bitmap1%29)も参照してください。

| 項目 | 受け付ける指定 |
| --- | --- |
| resource | `texture`または`surface`の片方 |
| 構成 | 単一mip、単一array、sample count 1、描画先として使える通常のresource |
| 色形式 | `B8G8R8A8_UNORM`または`B8G8R8A8_UNORM_SRGB` |
| alpha | `PREMULTIPLIED`または`IGNORE` |
| 寸法とDPI | resourceと一致する物理ピクセル寸法、正のDPI |
| 世代 | deviceとtargetそれぞれに非ゼロの世代 |

CPU読み戻し用resourceや別deviceのsurfaceを、そのまま描画先にすることはできません。
読み戻しはアプリが別のresourceを使って行います。

## フレームを描画して確定する

以下は、設定済みのtextureへ端末を描き、アプリケーションがその画素を採用する関数です。
作成済みのレンダラーと、配置・同期済みのUI表示先を渡します。
deviceとtextureの作成や、ウィンドウへの表示を含まない抜粋です。

```c
#include <boxrig/render/d3d11.h>

boxrig_renderer_status render_terminal(boxrig_d3d11_renderer *renderer,
    boxrig_ui_view *view, uint64_t device_generation,
    uint64_t target_generation)
{
    boxrig_renderer_frame_token token;
    boxrig_renderer_status status = boxrig_d3d11_renderer_prepare_frame(
        renderer, view, NULL, device_generation, target_generation, &token);
    if (status != BOXRIG_RENDERER_OK) return status;

    /* Keep this target's pixels as the completed frame. */
    return boxrig_d3d11_renderer_accept_frame(renderer, &token);
}
```

prepareが成功した時点では、再描画が必要な範囲はまだ保持されています。
結果を採用しない場合はacceptの代わりにabortを呼びます。
二重のacceptや古い世代のtokenは受け付けません。
描画から完了まで、同じ描画先への他のアクセスをアプリが排除してください。

既定では作成スレッドからだけ呼びます。
複数スレッド用の設定でも同時呼び出しはできません。
COMの参照はレンダラーが保持しますが、アプリのdeviceや描画先の最終参照を解放する前にレンダラーを破棄します。

## フォントと画像を扱う

フォント選択と文字配置には共通のDirectWrite実装を使います。
ファイルやメモリーのフォントを登録してから、resource IDをフォント指定へ渡せます。
ファイルはレンダラーの寿命中利用できる状態を保ち、メモリー登録では入力データがコピーされます。

DPIは描画先と文字描画設定を一致させます。
透過面ではClearTypeではなく、grayscaleなど実際に利用可能な方式を選びます。
COLR/CPALの層を使うカラー文字はDirectWrite 2の可否と方針に依存し、`color_layers`で確認できます。
SVGやbitmap形式のカラー文字も同じ能力であるとは扱いません。

画像の件数・バイト数、セル数には上限があります。
単一画像が上限を超える場合はフレームが失敗し、再描画要求を残します。
`get_statistics()`と`get_diagnostic()`で使用量、再描画量、有効な能力を確認できます。

## 描画先変更とデバイス喪失

サイズ変更では新しいresourceと増加したtarget世代を`set_target()`へ渡します。
通常の変更が失敗した場合は以前の描画先を維持します。
処理中のフレームがある間は変更できません。
同じレンダラーの描画先変更ではDPIを維持し、DPIが変わる場合は設定し直して生成します。

診断のlossがTARGETなら、新しい描画先を用意します。
DEVICEなら、アプリがdeviceとレンダラーを作り直します。
レンダラー自身がアプリのdeviceをresetすることはありません。
元のHRESULTと`device_reason`を診断として保持できます。

## 実行例と条件

[全体例](../../examples/win32/d3d11_renderer.c)は、描画先の画素を読み戻し、`boxrig-d3d11.ppm`へ保存します。
`--check`ではファイル保存を省略して、画素とフレームの受理を検証します。
この例は自分でdeviceを作るため、SDKのリンク指定に`-ld3d11`を追加します。

OSと検証環境の区別は[コンポーネントの環境条件](./d3d11-component.md#使える環境とsdk)と共通です。
必要なDirect2D interfaceを作れない場合、生成はHRESULTを返して失敗し、出力はNULLになります。

[公開ヘッダー](../../include/boxrig/render/d3d11.h)、[描画方式](./renderers.md)、[文書目次](./index.md)
