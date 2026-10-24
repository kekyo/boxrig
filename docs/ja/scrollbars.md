# スクロール位置とバーを扱う

[English](../en/scrollbars.md)

boxrig UIでは、過去の出力をスクロールして読み、現在の出力位置へ戻れます。
コンポーネントに付属する標準バーを使う方法と、アプリケーションのバーを接続する方法があります。
履歴の範囲と現在位置はboxrig coreが管理し、UIから取得・変更できます。

## スクロール位置を扱う

以下は、アプリケーションが独自に管理するUI表示先を、現在の出力位置へ戻す関数です。
作成・同期済みの表示先と、移動の有無を受け取る`bool`変数へのポインターを渡します。
標準コンポーネントのバーを使う場合は、[標準バーの設定](#標準バーを設定する)へ進んでください。

```c
#include <boxrig/ui.h>

boxrig_ui_status scroll_to_output(boxrig_ui_view *view, bool *changed_out)
{
    return boxrig_ui_view_apply_scrollbar_action(
        view, BOXRIG_SCROLLBAR_BOTTOM, 0, changed_out);
}
```

履歴の途中を見ていた場合は最新の位置へ移ります。
すでに同じ位置、履歴なし、alternate画面では何も変わりません。
結果は戻り値と`changed_out`の両方で確認します。

`apply_scrollbar_action()`は、位置を変更するとモデルの同期も行います。
アプリケーションは要求されたフレーム処理を進めて、新しい位置を描画します。
`get_scrollbar_state()`で、次の状態を取得できます。

| 状態 | 意味 |
| --- | --- |
| `history_rows` | 折り返し後の履歴の行数 |
| `viewport_rows` | 表示領域の行数 |
| `viewport_offset` | 現在の出力位置から履歴側へ戻った行数 |
| `position` | 最古を0、現在の出力位置を`history_rows`とする位置 |
| `enabled` | 現在の画面で履歴を移動できるか |
| `revision` | 状態が変わったときに更新する番号 |

先頭5項目は`state.model`内にあります。
同じモデルの表示先はスクロール位置とrevisionを共有します。
revisionの比較は同じモデルが生存している間だけ有効です。

## 履歴容量を変更する

primary画面に保存する履歴の上限は物理行単位です。`boxrig_terminal_set_scrollback_capacity()`で実行中に変更でき、現在の値は`boxrig_terminal_scrollback_capacity()`で取得します。
増やす場合は既存の履歴を保持し、減らす場合は最古の行から直ちに破棄します。0を指定すると履歴を消去します。
指定できる最大値は`BOXRIG_SCROLLBACK_CAPACITY_MAX`です。上限超過やメモリー不足で失敗した場合、容量、履歴、表示位置などは変更されません。
coreを直接変更した場合は、利用中のUIモデルを同期してからフレーム処理を進めてください。

## 標準バーを設定する

表示方針はALWAYS、AUTO、NEVERです。
定数の接頭辞は`BOXRIG_UI_SCROLLBAR_`です。
既定のALWAYSは領域を常に確保し、移動できないときは無効表示にします。
AUTOは履歴移動できる場合だけ表示し、NEVERは幅を本文へ返します。

各コンポーネントの`set_scrollbar_visibility()`で設定し、`get_scrollbar_visibility()`で要求と実際の配置を取得します。
`pending`が真なら配置や行列数の変更が未完了です。
バーを隠しても履歴は消えませんが、本文の列数が変わる場合があります。

X11とWin32では生成設定の`scrollbar_presentation`でネイティブの子バーまたは描画面内のバーを選べます。
NONEで生成した場合、表示方針の変更だけで標準バーは追加されません。
Waylandの既定は描画面内のバーです。

GTKでは、端末ウィジェットを親へ追加する前に`create_scroll_container()`で標準コンテナーを生成します。
返るfloating referenceをアプリで引き受け、コンテナーを配置します。
作成後のGTK側の表示方針を独自に上書きせず、boxrigの設定を使ってください。

## アプリ側のバーを接続する

フレーム処理で状態を読み、revisionが変わった場合だけバーの範囲と位置を更新します。
coreを直接変更した場合は、先にモデルを同期します。
状態取得だけでは位置変更や再描画情報の消費は行いません。

バーの操作はLINE_UP／DOWN、PAGE_UP／DOWN、TOP、BOTTOM、POSITIONへ変換します。
POSITIONの値は最古を0とする絶対位置です。
他の操作では位置引数を使いません。

バーへ状態を反映した通知が、再び端末への操作にならないようにしてください。
[X11](../../examples/x11/external_scrollbar.h)、[Win32](../../examples/win32/external_scrollbar.h)、[GTK 3](../../examples/gtk3/external_scrollbar.h)、[GTK 4](../../examples/gtk4/external_scrollbar.h)、[Wayland](../../examples/wayland/external_scrollbar.h)の接続例には、この処理が含まれます。

## 配置と倍率を変更する

標準バーが確保する幅はX11とWin32では画素、GTKとWaylandでは論理画素です。
native Waylandの既定は14論理画素です。
外部バーでは、配置と倍率をアプリ側で管理します。

変更が競合した場合は`BUSY`を返し、新しい方針を受け付けません。
受け付けた後は、通常のイベント・フレーム・行列数変更を完了させます。
ドラッグ中にバーを隠すと、そのドラッグは終了し、残りの解放イベントを端末入力へ転送しません。

## デモで確かめる

`BOXRIG_SCROLLBAR=auto`などを指定して[付属端末](./boxrig_term.md)を起動できます。
明示した場合はCtrl＋Shift＋F9でALWAYS、AUTO、NEVERを切り替えます。
未設定ではALWAYSで、このショートカットを取り込みません。

独自に描くつまみには`boxrig_scrollbar_calculate_thumb()`と`position_from_thumb()`を使えます。
定義と各状態の意味は[公開ヘッダー](../../include/boxrig/ui/scrollbar.h)を参照してください。

[文書目次](./index.md)
