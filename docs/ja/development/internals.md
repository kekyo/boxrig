# 内部設計を調べる

[English](../../en/development/internals.md)

この文書は、boxrigを変更する開発者が、変更対象のモジュールと依存先を調べるための資料です。
coreの独立性を保つ境界、端末状態と描画資源の管理、環境別実装の接続点を説明します。
利用するアプリケーションが理解すべき役割と所有権は[基本概念](../architecture.md)、公開APIは[API索引](../reference/api.md)を参照してください。

## モジュールと依存方向

coreは端末状態と制御列を担当し、OSのウィンドウ、PTY、フォントを所有しません。
共通UIは公開core APIを使い、環境別の入力とrendererが共通UIへ接続します。
sessionはcoreと非ブロッキングtransportをつなぎます。

```text
application
  ├─ X11 / GTK / Wayland / Win32 ─ UI ─ core
  └─ POSIX / Win32 transport ─ session ─ core
```

utf8procの型と関数はcore内へ閉じます。
共通UIと環境別実装は`src/core`やutf8procのinclude pathなしでコンパイルします。
インストールした公開成果物だけで独自UIを作る試験と、archiveの未定義symbol検査でこの境界を確かめます。

Unicode処理のutf8proc、PNG画像のlibpngとzlibは、coreの静的ライブラリへ組み込みます。
coreの独立性は、OSのウィンドウや入出力デバイスへ接続せずに端末状態を扱えることを指します。
依存ソースの取り込み方は[Makefile](../../../Makefile)、配布時の扱いは[配布条件](../reference/distribution.md)で確認できます。

## 解析と端末状態

[parser.c](../../../src/core/parser.c)はバイト列の状態機械、[screen.c](../../../src/core/screen.c)は画面、カーソル、属性、余白、編集を担当します。
UTF-8と制御列の途中状態を保持するため、入力を任意の位置で分割しても同じ結果を得られます。

端末生成時に容量と文字幅の設定を指定し、上限超過、部分消費、送信待ちを結果として返します。履歴容量は生成後も変更でき、文字幅など生成時に定める設定とは区別します。
OSC／DCS／APCは上限と終端を持つ処理として扱い、未知・不正な指定を読み終えて通常解析へ戻ります。

文字のクラスター分割と本来の列幅はcore、font face、glyph、整形、双方向表示とピクセル幅はUIとrendererが担当します。
IMEの未確定文字も公開の文字測定APIを使い、UTF-8と確保が全て成功してから状態を交換します。

## 文字列・画像・再描画情報の管理

画面の物理行と、折り返しを結合した論理行を区別します。
履歴破棄、行の分割・結合、リサイズ後も同じ文字を指すかは、行の識別子とrevisionで判断します。
保持した位置が失効した場合はSTALEです。

本文の射影、選択、検索、監視、認識は共通の文字位置を使います。
検索は処理量を制限して再開し、監視の初期走査は変更されないスナップショットを使います。
走査中の将来イベントは固定容量で保持します。
内容hashの再計算は変更した論理行へ絞り、全履歴のセルを毎回読み直しません。

正規表現認識は、行末探索、UTF-8の復号、境界の解決、NFAの準備も処理量に計上します。
無一致の全履歴をcurrent matchとして保持しません。
処理量と保持量を調べる方法は[文字処理の測定](../text-workload-measurements.md)を参照してください。

画像の復号と登録はtransactionとして扱い、成功前に有効な画像を置き換えません。
画像資源と配置を分け、共有資源のrevisionと描画順を使って必要な領域を無効にします。
アニメーションは単調増加時計から現在frameを求め、遅れた分の全frameを描画しません。

## 入力・ホスト要求・送信の順序

環境別adapterはOSのイベントから共通の論理キー・ポインター・IME入力を作ります。
ショートカット、選択、リンク、子プロセスへの入力の送り先を決め、同じ操作を二重送信しません。
送信イベントは原子的に追加し、WOULD_BLOCKなら同じ操作を再試行できます。

ホスト要求はfeed中にOS APIを呼ばず、型付きキューから取り出します。
OSC 52の応答待ちは送信順序を保持するための境界を持ち、後のキー入力が先行しません。
要求ID、consume、完了の順を検査し、一度だけ完了させます。

通知はcoreで断片を組み立て、policyで表示条件と頻度を判断し、providerでOSへ送ります。
非同期dispatcherが要求を保持し、取消・破棄でも完了を一度にします。
外部画像も同様にホストへ処理を委ねます。

## UIと描画資源の管理

モデルはcoreの変更を同期し、複数の表示先が独立した装飾・描画状態を持ちます。
端末の行列やプロトコル用セル寸法を決めるlayout authorityは一つだけです。
追加の表示先はその状態を変更せず、再配置できない条件を拒否します。

フレーム処理はモデルの変更、配置、描画資源、提示を区別します。
完成画面を保持できる場合はpresentだけを再実行し、文字を再rasterしません。
生成候補と確定済みgenerationを分け、失敗やretryで未完成の状態を確定しないようにします。

GL context、D3D device、出力先の寿命とgenerationは、描画資源の失効条件です。
失われたcontextへ解放コマンドを発行せず、復旧時に必要な資源を作り直します。
デバイス損失と、一時的な送信待ちや未配置を混同しません。

## 環境別実装の接続点

| 実装 | 所有するものと接続点 |
| --- | --- |
| X11／Win32 component | 子ウィンドウまたは借用先、入力adapter、renderer。アプリは親・イベントループ・processを所有 |
| GTK widget | GTKの配置とframeへ接続。破棄時にcallbackを止め、借用するモデルと端末はアプリが後で解放 |
| native Wayland | 非同期subsurfaceと描画buffer。displayのread／flush、親roleとcommitはアプリ側 |
| POSIX session | 非ブロッキングfdの待機条件をアプリへ返す |
| Win32 session | 同期ConPTY pipeを専用の読み書き処理へ接続。端末状態を変えるのはdispatchするアプリthread |

borrowed targetでは出力先全体の寸法とcomponentのviewportを分けます。
ポインターはviewport原点を引き、IME候補は原点を加えるため、原点が0でない配置でも描画と入力が一致します。
バーの幅を引く処理もcomponent内の一度に限ります。

破棄時はnative callback、保留中のclipboardやIME、描画の参照を停止してから所有資源を解放します。
caller所有のdisplay、親、モデル、端末をcomponentから破棄しません。
共通の[frontend fixture](../../../tests/frontend_component_fixture.c)と[対応行列](../../../tests/frontend_conformance_matrix.def)で各実装の共通条件を確認します。

## 変更時に確認する資料

公開APIを変える場合は、[API索引](../reference/api.md)から対象のヘッダーを調べ、現在の実装と機能テストを照合します。
検索変換の制約と公開機能との違いは[試作評価](../search-capability-prototype.md)、変更後の確認方法は[テスト手順](./testing.md)を参照してください。

公開ヘッダーの説明、文書の利用例、テストの期待する動作を、変更後のAPIにそろえてください。

[ドキュメント目次](../index.md)
