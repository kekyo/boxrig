# 文字とカーソルを点滅させる

[English](../en/blinking.md)

boxrig UIでは、文字とカーソルの点滅を有効にしたり、それぞれの間隔を変えたりできます。
設定は表示先ごとに持つため、同じ端末を点滅する画面と固定表示の画面へ同時に表示できます。

標準コンポーネントでは、同じUIループで共有するアニメーションruntimeが時計と次の通知を管理します。
独自のレンダラーには手動で時計を進めるAPIもあります。

## 点滅を有効にする

以下は、カーソルの表示時間を600 ms、非表示時間を400 msへ変更する関数です。
アニメーションruntimeへ接続済みのUI表示先を渡し、通常のイベントとフレーム処理を続けます。
runtimeの準備は[標準コンポーネントへの接続](#標準コンポーネントへ接続する)、独自に時計を進める場合は[待機処理への接続](#独自の待機処理へ接続する)を参照してください。

```c
#include <boxrig/ui.h>

boxrig_ui_status set_cursor_timing(boxrig_ui_view *view)
{
    boxrig_ui_blink_config blink = boxrig_ui_blink_config_default();
    blink.cursor.visible_milliseconds = 600;
    blink.cursor.hidden_milliseconds = 400;
    return boxrig_ui_view_set_blink_config(view, &blink);
}
```

端末とstyleが点滅するカーソルを指定している場合、表示と非表示がこの間隔で切り替わります。
文字はSGR 5がslow、SGR 6がrapid、SGR 25が解除です。
点滅を無効にすると文字やカーソルを固定表示しますが、端末が明示した非表示は解除しません。

## 間隔とフォーカス時の動作

既定では文字とカーソルの点滅が有効です。
slowとカーソルは500 msずつ、rapidは150 msずつです。
各時間は正数で、組の和が`uint64_t`に収まる必要があります。

カーソルは移動、ローカル入力、フォーカス復帰で表示位相へ戻ります。
フォーカスを失うと表示を固定します。
文字の点滅はフォーカスと独立して続き、表示先を非表示にすると止まります。
再表示すると表示位相から再開します。

文字の非表示位相では、文字、下線、取り消し線が消えます。
背景、画像、選択や検索の枠、コピーする本文は変わりません。

## 標準コンポーネントへ接続する

| 環境 | 接続方法 |
| --- | --- |
| X11 | `boxrig_x11_animation_poll_create()`でadapterを作り、configの`animation_runtime`へ渡す |
| Wayland | `boxrig_wayland_animation_poll_create()`を同様に使う |
| GTK 3／4 | ウィジェットが作成スレッドのmain contextへ自動接続する |
| Win32 | コンポーネントが作成したUIスレッドの通知へ自動接続する |

X11とWaylandでは、点滅を有効にしたままruntimeを渡さない生成は`UNAVAILABLE`です。
最初の固定文字列の例のように点滅を使わない場合は、生成設定でtextとcursorの両方を無効にできます。

poll adapterからは`boxrig_ui_animation_poll_runtime()`でruntimeを取得します。
待機直前に`prepare_wait()`で次の期限をtimeoutへ反映し、待機後に`dispatch()`で処理します。
複数のコンポーネントで共有し、全コンポーネントを破棄してからadapterを解放します。

## 独自の待機処理へ接続する

独自schedulerでは時刻取得、絶対期限の一回限りの予約、解除を実装します。
予約中に同期dispatchを呼ばず、通知は所有UIスレッドへ配送します。
予約のgenerationをそのまま返し、解除済みの通知は生存中の登録を確認してから扱います。

[手動時計の例](../../examples/ui/animation_manual.c)は、coreと表示先へ同じ単調時刻を渡して状態を確認します。
共有runtimeへ接続していない場合は、表示先の点滅とcoreの画像アニメーション・同期描画の両方の期限を取得し、最小の期限を待機へ反映してください。

```sh
make build/tests/animation-scheduler-example build/tests/animation-manual-example
build/tests/animation-scheduler-example
build/tests/animation-manual-example
```

[共有schedulerの例](../../examples/ui/animation_scheduler.c)では、二つの表示先、長い停止、非表示化、古い通知の処理を確認できます。

## 時計の寿命と失敗時の扱い

時刻には単調増加する絶対ミリ秒を使います。
同じ時刻は無作用、逆行はエラーです。
長い停止後に過去の点滅を全て再生することはなく、現在の位相へ進みます。

共有runtimeへ接続中の端末や表示先を、別のruntimeや手動時計でも更新しようとすると`BUSY`です。
設定変更時の予約失敗は以前の設定と予約を維持します。
実行中の予約失敗はruntimeのstatusとコンポーネントの診断で確認してください。

終了時は通知先を切り離し、表示先を外してからruntimeを解放します。
破棄済みのポインターへ遅延通知を配送しないでください。

[アニメーションAPI](../../include/boxrig/ui/animation.h)、[点滅API](../../include/boxrig/ui/blink.h)、[文書目次](./index.md)
