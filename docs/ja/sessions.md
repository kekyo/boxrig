# 端末と通信路をつなぐ

[English](../en/sessions.md)

シェルの出力を表示し、キーを押して操作するには、シェルと端末の間を通信路でつなぎます。
セッションAPIを使うと、PTYや独自の通信路から受信したデータをboxrig coreへ渡し、キー入力や端末の応答を相手へ送り返せます。
画面への表示にはboxrig UIを組み合わせ、通信だけを使う場合はcoreの結果をアプリケーションから取得します。

## セッションが担当する処理

セッションは受信、解析、送信待ちデータの書き出しを担当します。
ウィンドウのイベント待機、モデルの同期、描画はアプリケーションの仕事です。
1回の処理量には上限があり、大量の出力が続いても、イベントループへ制御を返します。

既定値は受信保持用バッファが4 KiB、1回のdispatchで読み込む上限が64 KiBです。
容量不足でcoreが受信を止めた場合、未処理データを保持します。
詳細は[公開ヘッダー](../../include/boxrig/session.h)を参照してください。

## POSIX PTYにつなぐ

次は、作成済みの端末へ`sh`を接続する関数です。
端末の初期サイズも24行80列にしてください。
成功時に返るPTYとセッションは呼び出し側で保持し、終了時に解放します。

```c
#include <boxrig/posix.h>

int connect_shell(boxrig_terminal *terminal,
    boxrig_posix_pty **pty_out, boxrig_session **session_out)
{
    char *arguments[] = { "sh", NULL };
    boxrig_session_config config = boxrig_session_config_default();
    *pty_out = NULL;
    *session_out = NULL;

    /* Start the shell and attach its PTY to the existing terminal. */
    if (boxrig_posix_pty_spawn(arguments, "xterm-256color", 24, 80,
            pty_out) != 0) return -1;
    if (boxrig_posix_pty_session_create(terminal, *pty_out, &config,
            session_out) != BOXRIG_SESSION_OK) {
        boxrig_posix_pty_destroy(*pty_out);
        *pty_out = NULL;
        return -1;
    }
    return 0;
}
```

この関数は、生成済みのcoreへ通信を接続する部分の抜粋です。
ウィンドウとイベントループを含む実行例は、[UI向けstep-by-step](./step-by-step/step-by-step-ui.md)を参照してください。

この時点ではシェルの出力を表示していません。
`boxrig_posix_pty_descriptor()`の読み書き可能状態を監視し、後述のdispatchを呼びます。
作成時に渡す`TERM`は子プロセスの環境へ設定されます。
ライブラリが`BOXRIG_TERM`を読むわけではありません。

ビルドにはcore、UIを使う場合はUI、session、POSIXの各ライブラリとPTYのリンク依存が必要です。
[ビルドガイド](./building.md)と[POSIXの利用例](../../examples/boxrig_term/common/posix_application.c)で実際の接続を確認できます。

## POSIXの起動結果と終了結果を分ける

`boxrig_posix_pty_spawn()`が`0`を返した時点で確定するのは、子プロセスを生成できたことです。
`boxrig_posix_pty_get_spawn_result()`で、端末の準備と`exec`の結果を取得してください。
この取得は待機せず、子プロセスも回収しません。

| 起動状態 | アプリの処理 |
| --- | --- |
| `PENDING` | `boxrig_posix_pty_spawn_descriptor()`の読み取り可能・切断を待ち、再取得 |
| `SUCCEEDED` | 起動診断の監視を終了。以後の稼働・終了は別に監視 |
| `FAILED` | `stage`と`error_number`で失敗を処理し、子の回収を完了 |

診断FDはPTYが所有し、破棄まで同じFDを保持します。
直接の読み取り、close、フラグ変更は行わず、状態が確定したら待機登録を解除してください。
成功判定には、[close-on-execを指定したpipe](https://www.man7.org/linux/man-pages/man2/pipe.2.html)の終了を使います。
起動成功はアプリケーションの準備完了や、その後も動作していることを保証しません。

親側の準備失敗も構造体で受け取りたい場合は、`boxrig_posix_pty_spawn_with_diagnostic()`を使います。
戻り値が`-1`なら`errno`と呼び出し元の診断へ原因を残し、PTYと子プロセスは返しません。
`0`なら初期診断は`PENDING`で、返されたPTYから続きの結果を取得できます。
親側の準備は同期処理です。
環境変数とPATH探索に必要な領域も、fork前に準備します。
子のTERMは親のTERMを変更せずに設定され、PATH探索とshebangなしの実行可能スクリプトの扱いは`execvp()`相当です。
プロセスの環境変数を別のスレッドで変更する場合は、この起動呼び出しと変更処理を直列化してください。

不存在のコマンドは`FAILED`・`EXEC`・`ENOENT`、起動後に127で終了するコマンドは`SUCCEEDED`です。
どちらも子の終了値が127になるため、終了値だけでは区別できません。
子の回収は`boxrig_posix_pty_wait()`かアプリの`waitpid()`のどちらか一方へ集約します。
最終診断は子の回収後も取得でき、PTYの破棄ではmasterと診断FDを閉じます。

## Win32 PTYにつなぐ

Windowsでは[PTY API](../../include/boxrig/win32/pty.h)で疑似コンソールを作り、`boxrig_win32_pty_session_create()`へ渡します。
[実行可能な例](../../examples/win32/pty.c)には、生成失敗の診断、子プロセス、送受信、終了までが含まれます。

Win32セッションには専用の`boxrig_win32_pty_session_dispatch()`があります。
待機には`boxrig_win32_pty_session_activity_handle()`を使い、ウィンドウのメッセージと合わせて待ちます。
読み書き用ワーカーはライブラリが管理しますが、端末状態の更新はdispatchを呼ぶスレッドで行います。

OS付属と同梱版のConPTYの選択はPTYの設定です。
診断には選択した方式、試行段階、失敗理由が残ります。
生成に成功したかどうかを、Windowsのバージョンだけで推測しないでください。

起動診断と終了結果は別々に保持します。
`boxrig_win32_pty_spawn_with_config()`が返す診断には、失敗段階、Win32エラー、ConPTYが返したHRESULT、fallbackの経過が含まれます。
成功が示すのは`CreateProcessW()`の成功までで、アプリケーションが利用可能になったことや、その後も実行中であることは保証しません。
[Microsoftの生成結果の契約](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)も参照してください。

終了の監視には、`boxrig_win32_pty_process_handle()`で取得した借用HANDLEを使います。
Cardioの`from_win32_handle()`などでsignaledを確認してから、`GetExitCodeProcess()`の終了値を取得してください。
通常終了127は起動失敗ではありません。終了値259も、signaledであれば実際の終了結果です。
259を`STILL_ACTIVE`と比較するだけでは、終了後も待ち続けることがあります。
[Microsoftの終了値の説明](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getexitcodeprocess)にある注意点です。
待機を完了または取り消してからPTYを破棄し、借用HANDLEを直接閉じないでください。

起動と終了を分けて確認するドライバーは、[POSIX](../../tests/test_posix_spawn.c)と[Win32](../../tests/win32/test_pty_spawn.cpp)にあります。
これらのC APIでは、起動と失敗時の後始末は同期処理です。

## GUIから起動と中止を行う

[C++20の起動ドライバー](../../examples/pty_spawn/main.cpp)は、StartとCancelで子プロセスを操作する実行例です。
起動失敗、子の終了、中止後の資源回収を個別に確認できます。
子の出力は読み取ってバイト数を表示し、端末画面や対話入力は提供しません。

```sh
make example-pty-spawn
build/examples/pty_spawn /bin/sh -c 'read value'
# 1回実行し、終了結果を表示して閉じる
build/examples/pty_spawn --once /bin/sh -c 'exit 127'
```

Linux版はGTK3を使い、子の終了を[pidfd](https://man7.org/linux/man-pages/man2/pidfd_open.2.html)で待ちます。Linux 5.3以降と、`sys/pidfd.h`を提供するlibcが必要です。
MinGWのamd64/i686版は`make example-pty-spawn-win32`で生成します。
Windowsではコマンドライン全体を1引数として渡します。
例えば`pty_spawn.exe --once "cmd.exe /d /c exit 259"`です。このGUIはシステム版ConPTYを使います。

[操作の所有者](../../examples/pty_spawn/operation.h)は同時に1件だけ起動を受け付け、資源回収中の次の要求を拒否します。
Cancelは取消要求であり、停止完了ではありません。
開始済みの同期OS関数が戻るのを待ち、必要なら生成された子を終了させ、I/OとPTYの資源を回収してから`finished`を通知します。
取消前の遅い`ready`通知は世代の照合で除外されます。
ウィンドウを閉じても、dispatcherを終了できるのは回収完了後です。

同期処理には[Cardioの`promises::start_new()`](../../deps/cardio/README_ja.md)を使います。
Win32で出力を回収する場合は、別の読み取り操作が同期pipeを読み続ける間にConPTYを閉じます。
出力を破棄する場合は、PTYのdestroyが出力pipeを先に閉じる順序です。
この区別は、古いWindowsで`ClosePseudoConsole()`が出力の排出を待つ場合にも必要です。
[Microsoftの終了処理の説明](https://learn.microsoft.com/en-us/windows/console/closepseudoconsole)を参照してください。

## 独自の通信路につなぐ

ソケットなどを使う場合は、非ブロッキングの読み書き処理を`boxrig_session_transport`の`read`と`write`へ登録します。
戻り値と転送量の意味は次のとおりです。

| 結果 | 転送量 | アプリケーション側の意味 |
| --- | --- | --- |
| `BOXRIG_TRANSPORT_OK` | 1バイト以上 | 実際に転送できた範囲だけを返す |
| `BOXRIG_TRANSPORT_WOULD_BLOCK` | 0 | 次の読み書き可能通知まで待つ |
| `BOXRIG_TRANSPORT_CLOSED` | 0 | 相手がその方向を閉じた |
| `BOXRIG_TRANSPORT_ERROR` | 0 | 継続できない通信エラー |

コールバック内で待機しないでください。
`resize`は任意です。
`destroy`を登録するとセッションが通信路のcontextを所有し、破棄時に一度呼びます。
登録しない場合は借用となるため、contextをセッションより長く生存させます。

## 待機と送受信を進める

1. `boxrig_session_interests()`から、読み込み、書き込み、即時dispatchの要求を取得します。
2. `INTEREST_DISPATCH`がある間は、待機に入る前にdispatchします。
   保持中のデータは`READY_NONE`でも再処理されます。
3. それ以外は、通信路とウィンドウのイベント、点滅などの期限を合わせて待ちます。
4. 実際の準備状態を`boxrig_session_dispatch()`へ渡します。
   `core_blocked`なら、送信キューやホスト要求を処理して空きを作ります。
5. 端末の更新後に`boxrig_ui_model_synchronize()`を呼び、コンポーネントを描画します。

`INTEREST_DISPATCH`の再処理だけで容量が回復するとは限りません。
ホスト要求や送信を進めずに同じ処理を繰り返すと、進展しないループになります。
Win32でもこの順番は同じですが、準備状態の引数を渡す代わりに専用dispatchを使います。

## リサイズと終了

確定した行列数は`boxrig_session_resize()`、Win32では`boxrig_win32_pty_session_resize()`で通信先と端末の両方へ反映します。
外部PTYのサイズ変更後にcore側の確保が失敗しても、外部の変更は取り消せません。
リサイズのエラーはセッションを継続できないものとして扱います。

`peer_closed`は、通信路の閉鎖だけでなく、保持中の受信と入力末尾の処理が終わったことを示します。
POSIXでは必要に応じて子プロセスをwaitしてからPTYを解放します。
セッションの破棄だけでは借用中のPTYを解放しません。

Win32のセッション破棄はワーカーを止め、疑似コンソールを閉じますが、借用中のPTYオブジェクト自体は別途破棄します。
どちらの環境でも、セッションを端末とPTYより先に破棄してください。
UI側の寿命は[基本概念](./architecture.md#所有権と終了)にまとめています。

[文書目次](./index.md)
