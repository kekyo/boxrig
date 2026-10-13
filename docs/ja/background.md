# 背景の色・画像・透過を設定する

[English](../en/background.md)

boxrig UIでは、端末の背後に色や画像を置き、対応する環境ではデスクトップを透過できます。
例えば、文字を不透明なまま残して背景だけを半透明にする表示を作れます。
OSへすりガラス効果を依頼する設定もあります。

| 表示したいもの | 使用する設定 |
| --- | --- |
| 端末の背後に置く単色 | 表示先の背景色 |
| スクロールで動かない背景画像 | 表示先の背景画像 |
| 背後のウィンドウやデスクトップ | コンポーネントの透過設定と親ウィンドウの準備 |
| ぼかしたデスクトップ | 対応OSの背景効果 |

## 背景色を設定する

以下は、作成済みのUI表示先へ半透明の赤を設定する関数です。
端末側の背景を透過した構成で呼び出し、通常のフレーム処理を進めると、文字の背後に赤が重なります。
表示先の作成は[UI向けstep-by-step](./step-by-step/step-by-step-ui.md)、透過の条件は[端末背景の透過](#端末背景を透過する)を参照してください。

```c
#include <boxrig/ui.h>

boxrig_ui_status set_red_background(boxrig_ui_view *view)
{
    return boxrig_ui_view_set_background_color(
        view, (boxrig_ui_rgba){255, 0, 0, 128});
}
```

この色は端末の下へ配置されます。
端末側の背景も透過する構成で、描画後の違いを確認してください。
alphaを0にすると追加した単色を見えなくできます。
文字色や端末パレットの変更ではないため、[色補正](./color-filters.md)とは別に設定します。

## 背景の重なり方

手前から、端末の文字や画像、追加した単色、背景画像、背後のウィンドウやデスクトップの順に重ねます。
各層はsource-overで合成します。
端末背景が不透明な場所では、その背後の色や画像は見えません。

色はalphaを乗算していない8ビットsRGBで指定します。
端末パレット、追加した単色、背景画像、OSの効果は、互いに設定を置き換えません。

## 背景画像を渡す

`boxrig_ui_background_image`へRGBA8の画素、幅、高さ、stride、利用できるバイト数、全体の不透明度、配置方法を設定し、`set_background_image()`へ渡します。
成功時に画素がコピーされるため、その後は元のバッファを解放できます。
失敗時は前の画像を維持します。

| fit | 結果 |
| --- | --- |
| `COVER` | 縦横比を保って領域全体を埋め、余分な部分を中央から切り取る |
| `CONTAIN` | 縦横比を保って全体を中央へ収め、余白を透明にする |
| `STRETCH` | 幅と高さをそれぞれ表示領域へ合わせる |

定数の接頭辞は`BOXRIG_UI_BACKGROUND_`です。
全体の不透明度と画素のalphaは一度だけ乗算します。
`NULL`を渡すと解除できます。
画像は表示先に固定され、端末のスクロールやKitty画像の消去では移動・消去されません。

保持量の既定上限は64 MiB、設定可能な最大値は256 MiB、各辺は最大16384画素です。
レンダラーの容量条件も満たす必要があります。
ファイルのデコードはアプリ側で行い、タイル配置や加算・乗算合成は現在のAPIにはありません。

## 端末背景を透過する

各コンポーネントのtransparency設定で、背景の不透明度と要求する範囲を指定します。
表示面のalphaと、トップレベルからデスクトップへの透過は別の能力です。
必須にするか、利用できる場合だけ使うかも選べます。

要求した値だけで成功と判断せず、`get_transparency()`などで有効な範囲と不透明度を確認してください。
親ウィンドウの準備は親の所有者が行います。
特にWin32の親の構成は[Win32ガイド](./platforms/win32.md#透過と親ウィンドウの条件)を参照してください。

<a id="すりガラス効果の要求"></a>

## OSのすりガラス効果を要求する

トップレベルの所有者が、環境別のbackground effect providerを作ります。
複数の端末が同じトップレベルを使う場合は、ホストが領域を集約します。
端末の文字やKitty画像そのものにぼかしを掛ける設定ではありません。

| 要求 | 結果 |
| --- | --- |
| `NONE` | ホストの要求を解除する |
| `PREFER` | 対応すれば要求し、未対応でも通常の透過を続ける |
| `REQUIRE` | 未対応なら`UNAVAILABLE`。Waylandの探索中は`BUSY`。失敗時は前の要求を維持する |

`get_state()`の`accepted`はOSやプロトコルが要求を受け付けたことを示します。
実際の画素がぼけたことの証明ではありません。
Waylandでは設定後に所有者のsurface commitも必要です。

## 未対応時の動作と配布条件

| 環境 | 接続条件 |
| --- | --- |
| Win32 | Windows 11 build 22621以降の[system backdrop](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type) |
| X11 | コンポジターが動作し、ルートウィンドウでKDE方式のぼかし能力を公開していること。[KDEの実装](https://github.com/KDE/kwindowsystem/blob/master/src/platforms/xcb/kwindoweffects.cpp)と同じ`_KDE_NET_WM_BLUR_BEHIND_REGION`プロパティを使用 |
| Wayland | [ext-background-effect-v1](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/blob/1.48/staging/ext-background-effect/ext-background-effect-v1.xml)のblur能力 |
| GTK 3 | 任意adapterからX11またはWaylandのproviderへ接続 |
| GTK 4/Wayland | 有効な経路には4.18以降の[commit要求API](https://docs.gtk.org/gdk4-wayland/method.WaylandSurface.force_next_commit.html)が必要 |
| GTK 4/X11 | 現在は未提供。[XID取得APIの非推奨化](https://docs.gtk.org/gdk4-x11/method.X11Surface.get_xid.html)を理由に標準接続を採用していない |

任意SDKのターゲットは`install-background-wayland`、`install-background-gtk3`、`install-background-gtk4`です。
GTKの通常ウィジェットへ、この依存を必須にはしていません。
GTK 4.14〜4.16でビルドしたadapterは利用不可を報告する構成です。

native X11の領域はdevice pixel、WaylandとGTK adapterでは論理座標です。
領域数0の意味も、native X11では全体、GTKとWaylandでは空領域と異なります。
providerはUIスレッドで使い、借用したネイティブ資源より先に解放してください。

[背景API](../../include/boxrig/ui/view.h)、[背景効果の状態](../../include/boxrig/ui/background_effect.h)、[デモの設定](./boxrig_term.md)、[文書目次](./index.md)
