# 端末に文字コード変換器を接続する

[English](../en/encoding-hooks.md)

boxrigは、画面や検索で扱う本文と、`boxrig_terminal_send_text()`に渡す文字列にUTF-8を使います。UTF-8を使う通信相手とは、既定の設定で多言語のテキストを送受信できます。
通信相手が別の文字コードを使うときは、その文字コードに対応した変換器を端末ごとに登録してください。

## 変換する範囲を選ぶ

端末が受け取るデータには、画面に表示する文字と、カーソルの移動などを指示する制御列が含まれます。制御列の開始に使うエスケープバイトを`ESC`（`0x1b`）と表記します。

| 通信相手の形式 | 設定 | 変換器に渡るデータ |
| --- | --- | --- |
| UTF-8、または接続層でUTF-8に変換済み | フックを登録しない | `boxrig_terminal_feed()`にUTF-8を渡す。受信が未完の文字で終わると、入力終了時に`�`（U+FFFD）を表示 |
| 本文とタイトルに別の文字コードを使い、端末の制御列はASCIIのまま送る | `BOXRIG_ENCODING_FIELDS` | 受信本文とタイトルを別々にUTF-8へ変換。送信するテキストや貼り付け本文は相手の文字コードへ変換 |
| 制御列も符号化する、または文字集合の切り替えにESCを使う | `BOXRIG_ENCODING_STREAM` | 制御列を含む受信ストリーム全体を先にUTF-8へ変換。送信では端末が生成するキー列、貼り付けの区切り、問い合わせ応答も変換 |

フィールド単位では、画面本文とタイトル（`OSC 0/1/2`）に印字可能なUTF-8を返してください。例えばISO-8859-1の本文の`E9`は`é`のUTF-8表現`C3 A9`に変換し、作業ディレクトリ通知（`OSC 7`）のUTF-8 URIやKitty画像のデータは元のバイト列で扱えます。タイトルの処理には拡張VTが必要です。

ストリーム単位では、端末が制御列を解釈する前に復号します。例えばUTF-16LEで届いた`1B 00 5B 00`を復号した結果は、制御列の先頭を表す`ESC [`です。

復号した文字の扱いは、アプリが選んだセル形式で決まります。例えば`é`をUnicodeセルに書くとそのまま保持され、ASCII・16色セルでの表示は1列の`?`です。[セル形式と端末制御の選び方](./core-features.md#必要な機能から構成を選ぶ)

## C99で変換器を登録する

次は送信先が7ビットASCIIを使う場合の例です。UTF-8の`café`を送ると通信相手には`caf?`が届きます。受信本文・タイトルでは印字可能なASCII以外を`?`に置き換えます。

```c
#include <boxrig/terminal.h>

#if BOXRIG_FEATURE_ENCODING_HOOKS
static size_t ascii_field(boxrig_encoding_channel channel, const uint8_t *input,
    size_t length, uint8_t *output)
{
    size_t count = 0;
    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = input[i];
        if (channel == BOXRIG_ENCODING_SEND && (byte & 0xc0u) == 0x80u)
            continue; /* One output byte per UTF-8 scalar. */
        if (output) {
            bool ascii = byte < 0x80 &&
                (channel == BOXRIG_ENCODING_SEND || (byte >= 0x20 && byte != 0x7f));
            output[count] = ascii ? byte : (uint8_t)'?';
        }
        ++count;
    }
    return count;
}

static boxrig_status convert_ascii(void *context, boxrig_encoding_channel channel,
    const uint8_t *input, size_t length, bool finish,
    boxrig_encoding_buffer *buffer, size_t *written)
{
    (void)context;
    if (finish) return BOXRIG_STATUS_OK; /* ASCII has no pending shift or input. */
    size_t needed = ascii_field(channel, input, length, NULL);
    uint8_t *output;
    boxrig_status status = boxrig_encoding_reserve(buffer, needed, &output);
    if (status != BOXRIG_STATUS_OK) return status;
    *written = ascii_field(channel, input, length, output);
    return BOXRIG_STATUS_OK;
}

boxrig_status attach_ascii(boxrig_terminal *terminal)
{
    const boxrig_encoding_hooks hooks = {
        .mode = BOXRIG_ENCODING_FIELDS, .convert = convert_ascii
    };
    return boxrig_terminal_set_encoding_hooks(terminal, &hooks);
}
#endif
```

端末を生成したら、最初の入出力の前に`attach_ascii(terminal)`を呼んでください。登録内容はboxrigがコピーします。状態を持つ変換器では`hooks.context`にアプリ所有の状態を渡し、登録の交換・解除が成功するか、端末を破棄するまで保持してください。`BOXRIG_FEATURE_ENCODING_HOOKS=0`のSDKではフックの型と関数を省くため、利用側でも例のように`#if`で囲みます。[公開APIとコールバックの型](../../include/boxrig/terminal.h)

## 分割入力と送信待ちを扱う

受信本文（`BOXRIG_ENCODING_RECEIVE`）、タイトル（`BOXRIG_ENCODING_TITLE`）、送信（`BOXRIG_ENCODING_SEND`）は独立した変換状態を使います。ストリーム単位の受信はタイトルも`RECEIVE`で扱います。受信の区切りは通信路次第です。複数バイトで表す文字の一部だけが届いたら、変換器に保持し、出力0バイトで成功を返せます。

`convert`が成功すると、その呼び出しの入力は全量を受け付けたことになります。出力先は`boxrig_encoding_reserve()`で1回確保し、実際に書いたバイト数を`*written`へ返してください。初期値は0です。状態を持つ変換器は、必要量を確保してから変換状態を進めます。エラーを返すときは呼び出し前の状態を保ってください。入出力ポインターはコールバックの間だけ有効です。C++の例外はコールバックの中で処理します。同じ端末に対する操作はコールバックから戻った後に行ってください。

| 呼び出しの結果 | アプリ側の処理 |
| --- | --- |
| `feed()`が入力を一部だけ消費 | `consumed_out`バイトだけ進め、送信待ちやホスト要求を処理して残りの受信バイトを再度渡す |
| 送信や終了処理が`BOXRIG_STATUS_WOULD_BLOCK` | `boxrig_terminal_output_peek()`で取り出して通信路へ書き、受理されたバイト数だけ`boxrig_terminal_output_consume()`で消費する。保留中のホスト要求があれば応答し、同じ操作を再試行 |
| 送信時の`BOXRIG_STATUS_LIMIT_EXCEEDED` | 1回の予約が設定上限を超えている。送信文字列を文字境界で小分けにするか、生成時の`output_capacity`を調整 |

生の受信ログは通信路で読み取った時点で一度だけ記録します。変換後のログや受信マクロには、任意の`hooks.received`を使えます。パーサーが受け付けたバイトを制御列も含めて順に通知するので、再試行で重複しません。X/Y/ZMODEMなどのバイナリ受信は通信路で識別して転送処理へ渡します。送信時はどちらの変換方式でも、`boxrig_terminal_send_bytes()`が指定バイトをそのまま送信キューへ追加します。

## 接続中に文字コードを切り替える

1. 新しい読み出しと送信操作をいったん止めます。旧文字コードで受信済みのデータを処理し、開始済みの貼り付けを終えます。通信セッションに保留入力がある場合は、キューの空きやホスト応答を用意して`BOXRIG_SESSION_READY_NONE`で処理を進めます。
2. `boxrig_terminal_finish_input()`を呼びます。変換器には`finish=true`、`input=NULL`、`length=0`が渡ります。未完の文字は変換器の方針で処理します。例えば文字の途中で入力が終わったときは、残ったバイトに対して`�`（U+FFFD）を返せます。終了したチャネルは次の入力を初期状態で受け付けます。
3. `boxrig_terminal_finish_encoding_output()`で送信側を確定し、必要な終端のシフト列も通信路に書き出します。`WOULD_BLOCK`なら上表の手順で再試行し、旧変換器の出力をすべて送ってください。
4. `boxrig_terminal_set_encoding_hooks()`に次の登録を渡します。`NULL`なら標準のUTF-8に戻ります。`BOXRIG_STATUS_BUSY`なら旧登録を保持したまま保留処理を終え、再試行してください。交換に成功すると、同じ画面と履歴の続きを新しい文字コードで表示できます。

チェックポイントを保存する前にも、この手順で登録を解除してください。アプリ所有の変換状態は保存対象外で、登録中のエクスポートは`BOXRIG_STATUS_BUSY`です。復元後は新たに変換器を登録してから通信を再開してください。[端末とセッションの引き継ぎ](./sessions.md#動作中の端末を別プロセスへ移す)

## 既存の接続を移行する

以前の`boxrig_terminal_config.encoding`と`BOXRIG_ENCODING_SHIFT_JIS`を使っていたアプリは、接続に合う変換器を用意して登録します。既に接続層で変換しているアプリをフックへ移す場合は、受信時に通信路から届いたバイトを直接`feed()`へ渡し、送信時はキューから取得したバイトをそのまま書き出してください。貼り付けの速度制限は、変換後に通信路へ書いたバイト数で計算します。

elder-termsの`TerminalTextStream`を実際に登録するC++の結合例があります。CP932で「日」と未完の先頭バイトを受け、UTF-8に切り替えて「語」を受けると、画面と変換後ログに残るのは「日�語」です。生ログには元の受信バイトが残ります。この例ではISO-2022-JPの分割受信、UTF-16LEの問い合わせ応答、2バイトずつ送る貼り付け、0～255の全バイト値の送信も確認できます。[boxrig側の接続例](../../tests/test_elder_terms_encoding.cpp)・[elder-termsの変換器](https://github.com/kekyo/elder-terms/blob/254bddc8816b38f2e74a8baa22c971492053fcf1/elder-terms-terminal/src/terminal-sessions/terminal-text-stream.cpp)

```sh
make test-elder-terms-encoding ELDER_TERMS_DIR=../elder-terms
```

旧SDKの作成手順で`data/encoding/whatwg/LICENSE`を指定していた場合は、SDKの入力一覧、インストール対象、配布物の検査からその参照を外してください。このファイルは、廃止したShift_JIS変換表に付属していたものです。[旧SDKの準備手順の例](https://github.com/kekyo/elder-terms/blob/254bddc8816b38f2e74a8baa22c971492053fcf1/scripts/prepare-boxrig.mts#L189)

[送受信の設定](./terminal-io.md)・[文書目次](./index.md)
