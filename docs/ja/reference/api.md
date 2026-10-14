# 公開APIの索引

[English](../../en/reference/api.md)

端末出力の解釈、文字列の検索、ウィンドウへの組み込みなど、行いたい操作から公開ヘッダーを探せます。
画面を使わない処理や独自の描画にはboxrig core、環境別の描画と入力受付にはboxrig UIを使います。
公開APIはC99で、C++からも呼び出せます。

最小のコードと結果は[READMEの実装例](../../../README_ja.md#実装例)、オブジェクト同士の関係は[基本概念](../architecture.md)を参照してください。
以下は用途別の索引です。
個々のAPIの条件はリンク先のヘッダーコメントで確認できます。

## coreと通信

| 用途 | 公開ヘッダー | ガイド |
| --- | --- | --- |
| 端末生成、解析、セル、履歴、送信、ホスト要求 | [terminal.h](../../../include/boxrig/terminal.h) | [core step-by-step](../step-by-step/step-by-step-core.md) |
| 文字列、選択、検索、監視、認識、画像、時計 | [terminal.h](../../../include/boxrig/terminal.h) | [文字列](../text.md)、[画像仕様](./graphics.md) |
| 非ブロッキングの通信セッション | [session.h](../../../include/boxrig/session.h) | [セッション](../sessions.md) |
| POSIXのfdとPTY | [posix.h](../../../include/boxrig/posix.h) | [セッション](../sessions.md) |
| Win32の通信とPTY | [session.h](../../../include/boxrig/win32/session.h)、[pty.h](../../../include/boxrig/win32/pty.h) | [Win32](../platforms/win32.md) |

## UIと入力

| 用途 | 公開ヘッダー | ガイド |
| --- | --- | --- |
| モデル、変更の同期 | [ui.h](../../../include/boxrig/ui.h) | [基本概念](../architecture.md) |
| 表示先、配置、フレーム、共通component | [view.h](../../../include/boxrig/ui/view.h)、[component.h](../../../include/boxrig/ui/component.h) | [UI step-by-step](../step-by-step/step-by-step-ui.md) |
| キー、ショートカット、貼り付け | [input.h](../../../include/boxrig/ui/input.h)、[shortcut.h](../../../include/boxrig/ui/shortcut.h)、[input_text.h](../../../include/boxrig/ui/input_text.h) | [送受信設定](../terminal-io.md) |
| IMEの未確定文字、候補、編集文書 | [composition.h](../../../include/boxrig/ui/composition.h)、[candidates.h](../../../include/boxrig/ui/candidates.h)、[editable.h](../../../include/boxrig/ui/editable.h) | [入力とIME](../input-ime.md) |
| 直接IMEの方式、接続状態、選択理由の診断 | [ime.h](../../../include/boxrig/ime.h) | [入力経路の選択](../input-ime.md#入力経路を選ぶ) |
| フォントと文字配置 | [text_layout.h](../../../include/boxrig/ui/text_layout.h) | [フォントと配置](../fonts-and-layout.md) |
| 時計、点滅、待機 | [animation.h](../../../include/boxrig/ui/animation.h)、[blink.h](../../../include/boxrig/ui/blink.h)、[animation_poll.h](../../../include/boxrig/ui/animation_poll.h) | [点滅](../blinking.md) |
| 色、背景画像、カーソル、選択、バー | [color.h](../../../include/boxrig/ui/color.h)、[image.h](../../../include/boxrig/ui/image.h)、[decoration.h](../../../include/boxrig/ui/decoration.h)、[scrollbar.h](../../../include/boxrig/ui/scrollbar.h) | [背景](../background.md)、[装飾](../decorations.md)、[バー](../scrollbars.md) |

## 環境別コンポーネント

| 環境 | 公開ヘッダーと説明 |
| --- | --- |
| X11 | [x11.h](../../../include/boxrig/x11.h)、[component.h](../../../include/boxrig/x11/component.h)、[組み込みガイド](../platforms/x11.md) |
| GTK 3 | [gtk3.h](../../../include/boxrig/gtk3.h)、[terminal_widget.h](../../../include/boxrig/gtk3/terminal_widget.h)、[組み込みガイド](../platforms/gtk.md) |
| GTK 4 | [gtk4.h](../../../include/boxrig/gtk4.h)、[terminal_widget.h](../../../include/boxrig/gtk4/terminal_widget.h)、[組み込みガイド](../platforms/gtk.md) |
| native Wayland | [wayland.h](../../../include/boxrig/wayland.h)、[組み込みガイド](../platforms/wayland.md) |
| Win32 | [win32.h](../../../include/boxrig/win32.h)、[component.h](../../../include/boxrig/win32/component.h)、[組み込みガイド](../platforms/win32.md) |

各環境の入力adapter、ホスト要求、透過の詳細も同じディレクトリにあります。
共通インターフェイスに設定関数があっても、全環境が同じ機能を実装するとは限りません。
[環境別対応](./platform-support.md)で確認してください。

## 描画・通知・背景の追加モジュール

OpenGLは[opengl.h](../../../include/boxrig/render/opengl.h)と[runtime.h](../../../include/boxrig/render/runtime.h)、Direct3D 11は[d3d11.h](../../../include/boxrig/render/d3d11.h)を使います。
環境別の子ウィンドウと、借用した描画先へ出力するrendererの違いは[描画方式](../renderers.md)を参照してください。

通知は[notification.h](../../../include/boxrig/ui/notification.h)、[notification_dispatch.h](../../../include/boxrig/ui/notification_dispatch.h)、[freedesktop.h](../../../include/boxrig/notification/freedesktop.h)で接続します。
背景効果は[background_effect.h](../../../include/boxrig/ui/background_effect.h)と各環境のproviderを使います。
利用方法は[ホスト連携](../host-integration.md)と[背景](../background.md)に記載しています。

## APIの読み方と結果コード

公開定義は`include/boxrig/`にあります。
各ヘッダーのコメントに、引数、戻り値、借用期間、失敗後の状態を記載しています。
内部ヘッダーをアプリケーションから参照する必要はありません。

coreの`boxrig_status`は次のように扱います。
環境別APIは専用の結果型と診断も持つため、数値が同じであることを前提に変換しないでください。

| 状態 | 呼び出し側の扱い |
| --- | --- |
| OK | 処理が完了した |
| INVALID_ARGUMENT | 引数や設定の組み合わせを確認する |
| OUT_OF_MEMORY、LIMIT_EXCEEDED | メモリ不足、設定上限を確認する |
| WOULD_BLOCK | キューなどを進めてから、契約に従って未処理分を再試行する |
| INCOMPLETE | 現在の認識結果が未完成。走査や再走査を進めてから取得する |
| STALE | 保持した位置や範囲が古い。再取得する |
| BUSY | 別の所有者や未完了処理がある |

`feed()`では状態と消費済みバイト数を共に確認します。
送信イベントが`WOULD_BLOCK`のときは、そのイベント全体を再試行します。
この違いは[送受信設定](../terminal-io.md)と[セッション](../sessions.md)に例があります。

## 所有権と関連ガイド

生成した端末、モデル、表示先は、借用する側から先に破棄します。
通常は表示先、モデル、端末の順です。
外部のdisplay、親ウィンドウ、GL contextやD3D deviceは、それぞれの借用契約が終わるまで保持します。

セルやホスト要求から返るポインターは、次の変更後も有効とは限りません。
長期間保持する文字列にはスナップショットを使い、非同期の要求は完了・取消と破棄の順序を守ります。
[基本概念](../architecture.md)、[文字列](../text.md)、[ホスト連携](../host-integration.md)で具体的な寿命を説明しています。

[ドキュメント目次](../index.md)
