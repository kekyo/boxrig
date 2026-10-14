# boxrigドキュメント

[English](../en/index.md)

boxrigは、アプリケーションへターミナル機能を組み込むためのC99ライブラリです。
[README](../../README_ja.md)では、機能と表示例に続けて、UIとcoreの短い実装例を紹介しています。
この目次から、使いたい機能のガイド、環境別の組み込み手順、詳しい対応仕様へ進めます。

## 初めて使う

既存のウィンドウへ組み込む場合はboxrig UI、描画を自分で実装する場合や画面を使わない処理にはboxrig coreを使います。
[最初の利用まで](./getting-started.md)で実行結果を確かめてから、step-by-stepで小さなアプリケーションを作れます。

| 文書 | 分かること |
| --- | --- |
| [README](../../README_ja.md) | boxrigでできることと短いコード例 |
| [最初の利用まで](./getting-started.md) | 構成を選んで例を実行する手順 |
| [ビルドとインストール](./building.md) | ソース、依存、SDK、リンクの準備 |
| [基本概念](./architecture.md) | core、UI、通信とアプリケーションの役割 |

## 機能を追加する

文字列の取得・検索や送受信の設定は、画面を使わないboxrig coreでも利用できます。
フォント、背景、IME、選択範囲の表示はboxrig UIで扱います。
各ガイドでは、使える機能とコード例を先に示し、その後で設定や条件を説明しています。

| 文書 | 分かること |
| --- | --- |
| [送受信設定](./terminal-io.md) | 改行、キーの送信列、文字コード |
| [文字列の取得・選択・検索](./text.md) | 本文の取得、完全一致検索、監視、認識 |
| [任意文字列の正規表現照合](./regex.md) | 端末を使わないUTF-8の照合、診断、分割実行 |
| [入力とIME](./input-ime.md) | 未確定文字、候補、アプリ所有文書の編集 |
| [フォントと配置](./fonts-and-layout.md) | フォント、文字サイズ、DPI、行列数 |
| [ホスト連携](./host-integration.md) | タイトル、クリップボード、通知、外部画像 |
| [背景](./background.md) | 背景色、画像、透過、OSの背景効果 |
| [表示色の補正](./color-filters.md) | 表示先ごとの色の変更 |
| [カーソルと選択範囲](./decorations.md) | 形状と見た目の設定 |
| [点滅](./blinking.md) | 文字とカーソルの点滅 |
| [スクロールバー](./scrollbars.md) | 標準バーとアプリ側のバー |

## アプリケーションに組み込む

[UI向けstep-by-step](./step-by-step/step-by-step-ui.md)では、固定文字列を表示するところから端末を作ります。
描画も自分で実装する場合は、[core向けstep-by-step](./step-by-step/step-by-step-core.md)を使ってください。

[boxrig_term](./boxrig_term.md)は、各環境の表示、入力、シェルとの通信を組み合わせたターミナル実装のコード例です。
表示や操作を試し、アプリケーションへ接続する処理をソースから確認できます。

| 文書 | 分かること |
| --- | --- |
| [通信セッション](./sessions.md) | PTYや独自の通信路の送受信、待機、終了 |
| [X11](./platforms/x11.md) | 親ウィンドウとイベントループへの接続 |
| [GTK 3／4](./platforms/gtk.md) | ウィジェットの配置とフレーム処理 |
| [native Wayland](./platforms/wayland.md) | 子surface、イベント待機、親のcommit |
| [Win32](./platforms/win32.md) | 子ウィンドウ、メッセージ、DPI |
| [描画方式](./renderers.md) | 標準描画、OpenGL、独自の描画先 |
| [Direct3D 11コンポーネント](./d3d11-component.md) | GPUで描画するWin32の子ウィンドウ |
| [Direct3D 11レンダラー](./d3d11-renderer.md) | アプリ所有のtextureへの出力 |

## 仕様を調べる

[対応状況](./compatibility.md)から、制御シーケンスと環境別の対応を調べられます。
[boxrig_emitter](./boxrig_emitter.md)は、確認用の制御列を出力するツールです。

| 文書 | 分かること |
| --- | --- |
| [制御シーケンス](./reference/terminal-sequences.md) | 受理する制御列、問い合わせ、入力プロトコル |
| [画像仕様](./reference/graphics.md) | Kitty graphicsの転送、配置、容量、応答 |
| [環境別対応](./reference/platform-support.md) | OS、描画、IME、通知などの条件と検証範囲 |
| [API索引](./reference/api.md) | 用途から選ぶ公開ヘッダー |
| [配布条件](./reference/distribution.md) | SDKの構成、実行時依存、同梱物とライセンス |
| [配布物向けREADME](../../README_pack.md) | SDKから最初の例を実行する方法 |

## boxrigの開発と検証

| 文書 | 分かること |
| --- | --- |
| [テスト手順](./development/testing.md) | 全体、機能別、SDK、GUIと実機の確認 |
| [内部設計](./development/internals.md) | モジュール、状態、送信順序、描画資源の管理 |
| [文字処理の測定](./text-workload-measurements.md) | 検索・監視・認識・コピーの処理量と保持量 |
| [検索変換の試作評価](./search-capability-prototype.md) | Unicode変換の採否と公開機能との違い |
