# 端末からホストへの要求を扱う

[English](../en/host-integration.md)

端末内で動くプログラムが送る制御列に応じて、ウィンドウタイトルを変えたり、クリップボードやデスクトップ通知を使ったりできます。
要求を解析するのはboxrig core、実際にOSを操作するのは端末を組み込むアプリケーションです。
描画や入力受付と同様に、どの操作を許可し、どうOSへ接続するかをアプリケーションで選べます。

## タイトルなどの要求を受け取る

`boxrig_terminal_host_item_peek()`で先頭の要求を読みます。
種類、flags、長さ付きのpayload、必要な場合はrequest IDを取得できます。
借用したデータは端末の次の変更まで有効なので、非同期で保持する場合は必要な内容をコピーしてください。

以下は、OS連携を提供しないアプリケーションで、先頭の一件を処理する関数です。
受信データを投入した後の端末を渡すと、要求をキューから取り出し、完了応答が必要な場合は利用不可と返します。
ウィンドウを持たないcoreだけの構成でも使えます。

```c
#include <boxrig/terminal.h>

boxrig_status discard_host_item(boxrig_terminal *terminal)
{
    boxrig_host_item item;
    if (!boxrig_terminal_host_item_peek(terminal, &item))
        return BOXRIG_STATUS_OK;

    /* Copy the ID before consuming the borrowed queue item. */
    uint64_t request_id = item.request_id;
    boxrig_status status = boxrig_terminal_host_item_consume(terminal);
    if (status != BOXRIG_STATUS_OK || request_id == 0) return status;
    return boxrig_terminal_complete_host_request(terminal, request_id,
        BOXRIG_HOST_REQUEST_UNAVAILABLE, NULL, 0);
}
```

タイトルやベルなど、完了応答を必要としない項目のrequest IDは0です。
タイトルにはウィンドウ／アイコンの対象と、保存・復元のflagsがあります。

## 行列数の変更要求を扱う

`CSI 8 ; rows ; columns t`は、`BOXRIG_HOST_ITEM_GRID_RESIZE`として通知されます。
`item.grid_resize`の型は、行と列それぞれの指定を保持する`boxrig_grid_size_request`です。
この値は直接コピーして保持できます。

| 指定 | mode | ホストでの解釈 |
| --- | --- | --- |
| 省略 | `BOXRIG_GRID_SIZE_KEEP` | 処理時の現在寸法を維持 |
| 正の整数 | `BOXRIG_GRID_SIZE_EXPLICIT` | `cells`で指定された行数・列数 |
| `0` | `BOXRIG_GRID_SIZE_DISPLAY` | ディスプレイ寸法から行数・列数を決定 |

例えば`CSI 8 ; ; 132 t`は行数を維持して132列を要求します。
省略と`0`の区別は[XTermの仕様](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html)に従います。
明示値は`1`から`UINT32_MAX`までで、coreは受信時に画面や履歴をリサイズしません。
アプリが許可と資源の上限を確認し、実際の行列数を決めてください。

共通UIでは、layout authorityを持つviewの`boxrig_ui_view_set_preferred_grid()`へ許可した行列数を渡せます。
既存のlayout actionとrevisionでウィンドウ寸法を調整し、確定したgridをcoreと通信路へ反映します。
拒否する場合はhost itemをconsumeするだけです。
request IDは`0`で、完了APIの呼び出しや端末への応答は不要です。
標準のX11・Win32ホストアダプターと付属端末は、通信先からのサイズ変更を適用せずに消費します。
独自の方針を使うアプリは、ホストキューを処理する際にこの項目を扱ってください。

host queueが満杯の場合は、通常の要求と同様に`feed()`が`WOULD_BLOCK`を返します。
キューを処理してから、`consumed_out`以降の未消費データを再投入してください。

## 応答と完了を返す

完了が必要な要求は、consumeした後で同じIDに一度だけ結果を返します。
成功、アプリの方針による拒否、環境として利用不可を区別できます。

読み取り要求は応答順を保つため、後続の送信を待たせる場合があります。
実行しない要求にも完了を返してください。
キューに残ったID、未知のID、完了済みのIDには完了を返せません。
成功時の読み取りデータには、要求が示す容量上限があります。

## クリップボードを扱う

OSC 52では読み書きの要求を受け取ります。
受信できる容量を設定することと、OS上のクリップボードへアクセスすることを分けて考えます。
アプリが利用目的に応じて許可し、読み取り結果または拒否を返します。

UIからの通常の貼り付けも非同期になり得ます。
終了時には、その完了通知が破棄済みの端末やウィンドウを参照しないように処理を停止してください。
標準コンポーネントの停止・破棄を使う場合は、各環境の契約に従います。

## リンクと作業ディレクトリを扱う

OSC 7のURIは通信先が報告した作業ディレクトリです。
OSC 8のリンクはセルに付随する情報です。
どちらも受信しただけで、ホストのディレクトリ変更やブラウザー起動が行われるわけではありません。

リンクを開く操作には、利用者のクリックなど、アプリが選んだ操作を接続します。
許可するURIの種類と起動方法もアプリで決めます。
付属端末の具体的な操作は[boxrig_term](./boxrig_term.md)を参照してください。

## デスクトップ通知を接続する

OSC 99の通知には、タイトル、本文、識別子、緊急度、フォーカスなどの表示条件があります。
coreの正規化済み要求を共通の[通知ディスパッチAPI](../../include/boxrig/ui/notification_dispatch.h)と、環境別のproviderへ接続できます。

Linuxの通知providerはGLib/GIOを使う任意モジュールです。
`make install-notification PREFIX=...`で配置し、`boxrig-notification`をリンクします。
GTKウィジェットを使うだけで通知providerまで必須にはなりません。
通知サービスがない場合の結果も処理してください。

## 外部画像データを許可する

Kitty画像のfileやshared memory転送は、coreが任意のファイルを直接開く機能ではありません。
`GRAPHICS_DATA`要求に、名前、offset、size、容量、読み取り後の削除要求などが入ります。
ホストは読み取りと削除の許可を判断してから、結果を返します。

[POSIXの補助API](../../include/boxrig/posix.h)を使う場合も、許可方針は呼び出し側の責任です。
データを読めたか、削除を認めたか、端末へ完了を返したかを混同しないでください。

## アプリケーションの許可方針を決める

利用者の環境へ作用する機能は、アプリの用途に合わせて個別に許可します。
例えば、ログ表示専用ならタイトルだけを反映し、クリップボード読み取りと外部画像データを利用不可にする構成も取れます。

coreの容量制限は、ホストの許可に代わるものではありません。
また、付属端末が選ぶ初期設定はライブラリ全体の方針ではありません。
保持中の要求、未送信応答、終了の順序は[通信セッション](./sessions.md)と合わせて実装してください。

[ホスト要求のAPI](../../include/boxrig/terminal.h)、[文書目次](./index.md)
