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

## 別の親surfaceへ移す

`boxrig_wayland_terminal_component_request_reparent(component, parent, &revision)`で、同じ接続の親surfaceへ移設を要求できます。
初回の配置を終えたコンポーネントが対象です。
共有メモリー描画とOpenGLの両方で、子surface、view、選択、表示設定、端末と通信セッションを保持します。
親には有効なsurfaceを渡し、自身や子孫を指定しないでください。

`OK`は要求の受理を表します。
受理時に旧親との関係を解除するため、旧親はこの時点で破棄できます。
新しい親は、移設完了後も次の移設要求の受理かコンポーネントの破棄まで生存させてください。
子surfaceは移設中に一時的に非表示になります。
親子関係とcommitの条件は、[Waylandのsubsurface仕様](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_subsurface)に従います。

通常の`frame()`を進め、`take_host_action()`が返した`action.parent`をcommitします。
その要求のrevisionを`complete_parent_commit()`へ返し、キューとcontextのdispatchを続けてください。
診断の`effective_reparent_revision`が要求時のrevisionに一致すると、サーバーが親変更を処理済みです。
画面への提示完了とは区別してください。
古いrevisionの完了通知では、新しい移設は完了しません。

入力・描画・配置処理中や別の移設中は`BUSY`を返し、旧状態を保持します。
キーやマウスの押下、未確定入力、貼り付けが終わってから再試行してください。
受理前の失敗では旧親を保持します。
受理後に新しい関係の準備が資源不足で失敗した場合は、要求を保持したまま`frame()`で再試行します。
接続切断時は通常の終了処理へ進んでください。

移設中の入力は破棄し、アプリが保留した旧世代の入力も再送しません。
完了を待つ間に指定した親へキーボードフォーカスが入った場合は、完了後に入力を再開します。
それ以外の場合は、アプリ側で移動先のフォーカスを設定してください。
移設後は、親への入力転送も明示的に指定した親だけを対象にします。
移動先の配置が決まったら`allocate()`へ渡し、行列数の変更を通信先へ反映します。

[往復移設の例](../../../examples/wayland/transfer.c)はF6で移動します。
`make build/examples/wayland_transfer`で共有メモリー版、`make build/examples/wayland_transfer_opengl`でOpenGL版を生成できます。

## 入力とクリップボード

既定のAUTOは、候補方針がCLIENT以外でtext-input-v3 version 2が利用できる場合に、コンポジター経由の入力を優先します。
それ以外ではIBusとfcitx 5／4を自動検出します。
アプリケーションからの入力経路の明示指定も可能です。
周辺文字列、候補表示、入力経路の結果はcontextとコンポーネントの診断から確認します。

クリップボード転送のFDは`prepare_wait_sources()`で取得し、準備状態を`dispatch_wait_sources()`へ渡します。
FDは借用なので閉じないでください。
Waylandまたは転送のdispatch後には待機対象を取り直します。
直接IMEの接続には`prepare_ime_poll()`と`dispatch_ime_poll()`も使います。

コンポーネントから端末へ送る入力を禁止するAPIは、`boxrig_wayland_terminal_component_set_input_enabled(component, false)`です。
禁止・再開のたびに入力世代が変わり、保留キー、IME確定、受信中のclipboard、未開始のpasteを無効にします。
禁止をまたいで押されているキーは、離すまで送信しません。
選択、コピー、履歴移動、ローカルのキー・マウス操作、描画、端末への問い合わせに対する応答は継続します。

開始済みpasteの終端を投入できない場合、禁止要求は`BUSY`を返し、状態は`STOPPING`になります。
core出力を通常どおり処理しながらcontextのdispatchを続けてください。
`get_input_state()`が返す状態が`DISABLED`になると、再開を要求できます。
停止完了は必要な終端をcoreへ投入した時点です。通信路への排出完了はホストが管理します。
入力処理中のコールバックから呼び出した場合も`BUSY`を返しますが、この場合は状態を変更しません。
禁止によって転送FDが無効になることがあるため、待機対象は取り直してください。

## OpenGLの選択と終了

OpenGL版は`boxrig-wayland-ogl`と[専用の生成API](../../../include/boxrig/wayland/opengl_component.h)を使います。
共通の入力、配置、親commitの責任は変わりません。

終了時は待機への登録とコールバックを止め、コンポーネントを破棄し、contextを解放します。
その後でアプリ所有のsurface、キュー、displayを解放してください。
ライブラリは借用したdisplayを切断しません。

[Waylandの公開API](../../../include/boxrig/wayland.h)、[デモの起動](../boxrig_term.md)、[文書目次](../index.md)
