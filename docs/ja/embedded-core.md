# 小型画面でシリアルログを表示する

[English](../en/embedded-core.md)

シリアル機器が送るログを小さな画面に表示する場合、必要な文字と制御列を選んでboxrig coreをビルドできます。受信バイトを渡すと、coreは文字の折り返し、スクロール、色、再描画が必要な範囲を管理します。フォントの用意と画素の描画はアプリケーションの役割です。

## 表示する文字と端末制御を選ぶ

| 表示したい内容 | ビルド時の指定 | 表示例 |
| --- | --- | --- |
| 日本語を含む端末アプリの画面 | 既定のUnicodeセルと拡張VT制御を使う | 全画面アプリが代替画面を使える |
| 日本語を含む計測ログ | Unicodeセルのまま`BOXRIG_FEATURE_EXTENDED_VT=0` | `日Z`を受け取ると「日」が2列を占め、「Z」が3列目に出る |
| 英数字と16色のシリアルログ | `BOXRIG_FEATURE_COMPACT_CELLS=1` | 同じ入力は`?Z`になり、2列に収まる |

セルは画面上の文字と表示属性を保持する単位です。小型セルはASCII文字に1バイト、前景色と背景色に各4ビットを使います。表示できないUnicode符号位置1個に割り当てるのは、1列の`?`です。通信相手の文字コードは別に選べます。例えばCP932の日本語を表示する場合は、Unicodeセルを選び、アプリ側の変換器で受信本文をUTF-8にしてください。

## 小型の静的SDKを作る

次のコマンドは、UTF-8で届く英数字のログ用に、変換フックを省いたcoreをビルドしてインストールします。ソースツリーのルートで実行してください。生成ツールの準備はビルドガイドにあります。[ビルドの準備](./building.md)

```sh
sdk="$PWD/build/serial-sdk"
flags='-DBOXRIG_FEATURE_COMPACT_CELLS=1 -DBOXRIG_FEATURE_ENCODING_HOOKS=0'
make install-core BUILD_DIR=build/serial CPPFLAGS="$flags" PREFIX="$sdk"
cc -std=c99 -I"$sdk/include" tests/test_embedded_log.c \
  "$sdk/lib/libboxrig.a" -lm -o build/serial/log-demo
build/serial/log-demo
```

最後のコマンドは端末にログを送り、折り返し、スクロール、色、行末消去と変更範囲を検査して`PASS`を出力します。静的ライブラリだけを作るときは、同じ`BUILD_DIR`と`CPPFLAGS`で`make core`を実行してください。[ログを送るCドライバー](../../tests/test_embedded_log.c)

ソースの`<boxrig/config.h>`には`#ifndef`で既定値が定義されています。`CPPFLAGS`で使える値は整数リテラルの`0`と`1`です。配布SDKにはビルド時の値を確定したヘッダーが入るため、利用側はこのヘッダーと同じSDKのライブラリを使います。機能判定は`#if BOXRIG_FEATURE_ENCODING_HOOKS`のように値で行います。SDKと矛盾する再定義はコンパイルエラーです。[設定ヘッダー](../../include/boxrig/config.h)

## 機能定義と既定値

表の名前にはすべて`BOXRIG_FEATURE_`を付けます。「小型」は`COMPACT_CELLS=1`を選んだときの値です。

| 定義 | 通常 | 小型 | `1`で利用できる機能 |
| --- | ---: | ---: | --- |
| `COMPACT_CELLS` | 0 | 1 | ASCIIと16色を保持する、履歴のない単画面 |
| `EXTENDED_VT` | 1 | 0 | 拡張VT制御とタイトルなどのホスト要求 |
| `LOG_CURSOR` | 0 | 0 | ログ表示にカーソルの移動・表示切り替えと画面消去を追加。拡張VT制御には標準で含まれる |
| `ENCODING_HOOKS` | 1 | 1 | 端末ごとに外部の文字コード変換器を登録 |
| `KITTY_GRAPHICS` | 1 | 0 | Kitty画像の転送・配置。libpngとzlibを使用 |
| `TEXT_MONITOR` | 1 | 0 | 指定した文字列の出現を監視 |
| `TEXT_RECOGNIZER` | 1 | 0 | 正規表現で端末上の文字列を認識 |
| `HISTORY_DEFLATE` | 1 | 0 | スクロール履歴をzlibで圧縮 |
| `CHECKPOINT` | 1 | 0 | zlibを使うチェックポイントで端末状態を保存・復元 |
| `ALT_SCREEN` | 1 | 0 | 主画面とは別に、全画面アプリ用の代替画面を確保 |

Unicode構成では各機能を個別に選べます。例えば`HISTORY_DEFLATE=0`でも、生成時に`scrollback_capacity`に行数を指定すれば圧縮せずに履歴を保持できます。監視・認識を外した構成でも、通常の文字列検索は有効です。画像、履歴圧縮、チェックポイントをすべて外すとzlibへの依存もなくなります。

小型セルと組み合わせられるのは`LOG_CURSOR`と`ENCODING_HOOKS`です。拡張VTや上表の6つの端末機能を明示的に有効にすると、設定ヘッダーでエラーになります。小型構成の公開APIには、端末の生成・リサイズ、受信、セルとカーソルの取得、テキストと生バイトの送信があります。標準UIやキー・貼り付けイベントを使うアプリはUnicode構成を選んでください。[端末の公開API](../../include/boxrig/terminal.h)

## ログで使える制御列

`EXTENDED_VT=0`では、復帰（CR）、改行（LF）、後退（BS）、水平タブ（HT）を扱います。行末で折り返し、画面下端でスクロールします。表の`CSI`はESCバイト（`0x1b`）と`[`の並びです。

| 用途 | 指定 |
| --- | --- |
| 文字色・背景色 | `CSI … m`。前景`30–37`、`90–97`、背景`40–47`、`100–107`。`39`と`49`で各色を既定に戻し、`0`で両方を戻す |
| 行の消去 | `CSI 0 K`でカーソルから行末、`CSI 1 K`で行頭からカーソル、`CSI 2 K`で行全体 |
| `LOG_CURSOR=1`での位置指定 | `CSI n A/B/C/D`で相対移動、`CSI n G`で列指定、`CSI row ; column H/f`で行・列指定。行・列は1から数える |
| `LOG_CURSOR=1`での表示・消去 | `CSI ? 25 h/l`でカーソルを表示・非表示。`CSI 0/1/2 J`でカーソルから画面末尾、画面先頭からカーソル、画面全体を消去 |

例えば`"\r\033[32mOK\033[K"`は、行頭に緑の`OK`を書き、その右側に残った古い表示を消します。タイトル指定（OSC）や機器制御文字列（DCS）は、ログ表示では終端まで読み飛ばします。Unicode構成でのKitty画像は、`KITTY_GRAPHICS`で選ぶ別の機能です。[通常の端末が扱う制御列](./reference/terminal-sequences.md)

## 変更のあったセルを描く

`boxrig_terminal_config_default()`で初期値を取得し、`columns`と`rows`を設定して端末を生成します。小型セルでは`scrollback_capacity`を既定の`0`にしてください。受信したバイトを`boxrig_terminal_feed()`へ渡し、処理したバイト数を確認したら、次の関数で変更範囲を描けます。`paint`には、指定位置に文字と背景色を描くアプリ側の関数を渡します。

```c
#include <boxrig/terminal.h>

void draw_log(boxrig_terminal *terminal,
    void (*paint)(size_t row, size_t column, const boxrig_cell_view *cell))
{
    boxrig_dirty dirty;
    while (boxrig_terminal_take_dirty(terminal, &dirty)) {
        for (size_t i = 0; i < dirty.column_count; ++i) {
            size_t column = dirty.column + i;
            boxrig_cell_view cell;
            if (boxrig_terminal_get_cell(terminal, dirty.row, column, &cell) == BOXRIG_STATUS_OK)
                paint(dirty.row, column, &cell);
        }
    }
}
```

APIで使う行と列の先頭は0です。小型セルでは`cell.text[0]`がASCII文字で、`foreground.index`と`background.index`が0～15の色番号です。消去された空白も背景色で塗り直してください。セルの文字を参照できるのは次に端末を変更するまでです。画面を全面的に描き直すときは`boxrig_terminal_invalidate()`、カーソルを重ねるときは`boxrig_terminal_get_cursor()`を使います。LovyanGFXやLVGLで描画する場合も、この`paint`に相当する処理を移植先で用意します。

## フラッシュとRAMの見積もり

Linux x86-64、GCC 13.3.0、`-O2`で、同じログを扱うCドライバーを静的リンクしたときの実測値です。各端末を80列×24行、履歴0行にそろえました。コードと読み取り専用データはリンク後のELFのセクションサイズ、ヒープは端末生成直後に確保中の要求バイト数です。

| 構成 | コード（`.text`） | 定数（`.rodata`） | 初期ヒープ |
| --- | ---: | ---: | ---: |
| 既定のUnicode端末 | 435,168 B | 385,492 B | 5,287,962 B |
| Unicodeログ、画像・監視・認識・履歴圧縮・チェックポイント・代替画面なし | 123,278 B | 348,340 B | 160,920 B |
| ASCIIログ、変換フックあり | 15,167 B | 872 B | 4,848 B |
| ASCIIログ、変換フックなし | 11,614 B | 872 B | 4,768 B |

40列×15行にすると小型セル600個で1,200バイトです。端末全体の初期ヒープはフックなしで1,984バイト、ありで2,064バイトでした。実機のRAM予算には、ピーク時の送受信データ、変換器、フォント、描画バッファ、アロケーターの管理領域も見込みます。`make test-embedded-log`でホストの測定を再現できます。[測定に使うドライバー](../../tests/measure_embedded_core.c)

[文書目次](./index.md)
