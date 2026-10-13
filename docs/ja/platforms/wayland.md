# native Waylandへ組み込む

[English](../../en/platforms/wayland.md)

boxrig UIのnative Wayland版を使うと、アプリケーションが所有するsurfaceの子として端末を表示できます。
端末の内容を管理するboxrig coreに、Waylandでの描画と入力受付を組み合わせた構成です。
GTKを介さずWaylandのイベントループを組みたい場合に使います。
display、専用イベントキュー、親surface、トップレベルの構成はアプリケーションが管理します。

## 必要なプロトコルとライブラリ

ビルドにはWayland、xkbcommon、Pango/Cairoと、リポジトリで固定しているwayland-protocolsを使います。
`make install-wayland PREFIX=...`でSDKを用意し、`boxrig-wayland`のpkg-config情報でリンクできます。
詳しくは[ビルドガイド](../building.md)を参照してください。

実行時に得られるプロトコルはコンポジターによって異なります。
必須機能がそろうまではcontextが初期化中となり、未対応の入力・倍率機能は診断から判別します。
プロトコルの定義は[固定したwayland-protocols](../../../deps/wayland-protocols)も参照できます。

## surfaceとコンポーネントを用意する

以下は、用意済みの親surface内へ端末を配置するために、子surfaceを作る関数の例です。
親surfaceと端末、UIモデルに加え、Waylandの接続情報を持つcontextを用意します。

`boxrig_wayland_context_create()`へ接続と専用キューを渡すと、非同期でregistryを調べ始めます。
生成関数自身はroundtripもイベント読み込みも行いません。
イベントを処理してcontextがREADYになってから、次の生成部分を呼びます。
`animations`には同じループで使う[アニメーションruntime](../blinking.md)を渡します。

```c
#include <boxrig/wayland.h>

boxrig_ui_status create_terminal_surface(boxrig_wayland_context *context,
    struct wl_surface *parent, boxrig_ui_model *model,
    boxrig_ui_animation_runtime *animations,
    boxrig_wayland_terminal_component **component_out)
{
    boxrig_wayland_terminal_component_config config =
        boxrig_wayland_terminal_component_config_default();
    config.animation_runtime = animations;
    return boxrig_wayland_terminal_component_create(
        context, parent, model, &config, component_out);
}
```

コンポーネントは子surfaceとsubsurfaceを所有します。
親surface、context、モデルは借用します。
現在のAPIは子surface方式であり、任意の既存surfaceへの描画を同じ生成APIで扱うものではありません。

## イベント待機と描画

専用キューのdispatch後に`boxrig_wayland_context_dispatch()`を呼び、受け取った通知を処理します。
通信による変更をモデルへ同期してから、コンポーネントの`frame()`を呼びます。

displayへの送信はアプリケーションがflushし、成功を`boxrig_wayland_context_note_flushed()`へ伝えます。
読み取り準備とキャンセルを含むWaylandの待機手順は、[実際のイベントループ](../../../examples/boxrig_term/wayland/event_loop.c)を参考にしてください。
display、PTY、クリップボード、IME、キーリピート、点滅の期限を一つの待機にまとめています。

`frame_pending`が真でも、`additional_dispatch`が偽なら、バッファのreleaseやframe callbackを待っている場合があります。
この間に描画を繰り返しても進みません。

## サイズ・倍率・装飾

`allocate()`へ渡す位置と寸法はsurfaceの論理座標です。
コンポーネントは出力倍率に合わせた物理ピクセルの描画先を用意します。
整数倍率と、利用可能な場合のfractional scaleを扱います。

子の配置変更では親のcommitが必要になる場合があります。
`take_host_action()`で取り出した要求に従い、アプリが親をcommitしてから`complete_parent_commit()`へ同じrevisionを返します。
boxrigが借用中の親を勝手にcommitすることはありません。

トップレベルの装飾はアプリ側の仕事です。
付属デモは利用可能な環境でlibdecorを使いますが、boxrigのコンポーネントを使うための必須条件ではありません。

## 入力とクリップボード

標準の入力経路ではコンポジター経由のtext-inputを使います。
直接IBusを使う設定もあります。
周辺文字列、候補表示、入力経路の結果はcontextとコンポーネントの診断から確認します。

クリップボード転送のFDは`prepare_wait_sources()`で取得し、準備状態を`dispatch_wait_sources()`へ渡します。
FDは借用なので閉じないでください。
Waylandまたは転送のdispatch後には待機対象を取り直します。
直接IBusの接続には`prepare_ime_poll()`と`dispatch_ime_poll()`も使います。

## OpenGLの選択と終了

OpenGL版は`boxrig-wayland-ogl`と[専用の生成API](../../../include/boxrig/wayland/opengl_component.h)を使います。
共通の入力、配置、親commitの責任は変わりません。

終了時は待機への登録とコールバックを止め、コンポーネントを破棄し、contextを解放します。
その後でアプリ所有のsurface、キュー、displayを解放してください。
ライブラリは借用したdisplayを切断しません。

[Waylandの公開API](../../../include/boxrig/wayland.h)、[デモの起動](../boxrig_term.md)、[文書目次](../index.md)
