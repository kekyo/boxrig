# 送受信する文字とキーを設定する

[English](../en/terminal-io.md)

Enterや貼り付けの改行、受信する改行、文字コードを通信相手に合わせるための設定です。
例えば、EnterでLFを送る機器との接続に利用できます。
改行とキーの初期値は端末生成時に指定します。外部の文字コード変換器は生成後に登録します。
Enter・消去キー・矢印キーの設定は、生成後にも一括で変更できます。
同じ端末を表示する複数の表示先は、同じ送受信規則を使います。
UIの色やフォントとは独立した設定です。

## 受信と送信の違い

受信時には、通信先から届いたバイト列を`feed()`へ渡します。
送信はキー、確定文字、貼り付けなどを送信キューへ追加する操作です。
キーを送っただけで画面を書き換えることはありません。

以下は、EnterをLF、貼り付けをCRLF、受信したCRLFを一回の改行にする端末を生成する関数です。
UIは使わず、通信処理を接続する前の設定部分を示しています。
成功時に返る端末をアプリケーションで保持し、使用後は`boxrig_terminal_destroy()`で解放します。

```c
#include <boxrig/terminal.h>

boxrig_status create_line_terminal(boxrig_terminal **terminal_out)
{
    boxrig_terminal_config config = boxrig_terminal_config_default();
    config.input.enter_newline = BOXRIG_NEWLINE_LF;
    config.paste_newline = BOXRIG_NEWLINE_CRLF;
    config.receive_newline = BOXRIG_RECEIVE_NEWLINE_CRLF;
    return boxrig_terminal_create(&config, terminal_out);
}
```

設定は生成時にコピーされ、RISやDECSTRでリセットしても保持します。
生成後に手元の`config`を書き換えても、端末には反映されません。

## Enterと貼り付けの改行

| 設定 | 既定値 | 選べる動作 |
| --- | --- | --- |
| `input.enter_newline` | `BOXRIG_NEWLINE_TERMINAL` | 端末のmode 20に従う、またはCR／LF／CRLFへ固定 |
| `paste_newline` | `BOXRIG_NEWLINE_LF` | 貼り付け本文のLFをCR／LF／CRLFへ変換 |

EnterのTERMINAL設定では、mode 20が無効ならCR、有効ならCRLFです。
固定設定はmode 20で変わりません。
Kittyのキー報告やapplication keypadのEnterは、それぞれのプロトコルに従います。

貼り付けではTERMINAL指定を受け付けません。
標準UIはクリップボード内のCRとCRLFをLFへ正規化し、coreが送信先の改行列へ変換します。
bracketed pasteの開始・終了列にはこの変換を行いません。
`send_text()`にも貼り付けの設定は適用されません。

## 受信CR／LF

指定値の接頭辞は`BOXRIG_RECEIVE_NEWLINE_`です。

| 値 | CRの結果 | LFの結果 |
| --- | --- | --- |
| `TERMINAL`、既定 | 同じ行の先頭 | 次の行。mode 20が有効なら先頭へ移動 |
| `STANDARD` | 同じ行の先頭 | 次の行の同じ列 |
| `LF` | 同じ行の先頭 | 次の行の先頭 |
| `CR` | 次の行の先頭 | 次の行の同じ列 |
| `CRLF` | 次の行の先頭 | 次の行の先頭。ただし直前のCRとの組は一回 |

CRLF設定では、CRとLFが別の`feed()`に分かれても隣接していれば一回です。
間にNULやESCを含む別のバイトがあれば二回になります。
VT、FF、IND、NELや制御文字列の内容には、この設定を適用しません。

[改行の実行例](../../examples/core/newline.c)は`make example-newline`で作れます。
`build/examples/newline`で送信列と受信後のカーソル位置を確認できます。
PTYを介さないため、OSの改行変換と区別して観察できます。

## キーの送信列を変更する

`input.key_mappings`には、上下左右のキーとmodifierに対する長さ付きのバイト列を指定できます。
表とバイト列は生成時と設定交換時にコピーされます。
長さ0はそのキーを送らない設定、NULを含む列も有効です。

対応するmodifierの組に完全一致した場合に使い、Caps LockとNum Lockは照合から除外します。
重複する設定は無効です。
上限は64項目、各4096バイト、合計65536バイトです。

指定列はlegacyのキー入力へ適用し、application cursor modeより優先します。
完成した列はAltのESCを追加せずに送ります。フィールド単位の変換では登録したバイト列をそのまま送り、ストリーム単位では他の制御列とともに送信フックを通します。[変換する範囲を選ぶ](./encoding-hooks.md#変換する範囲を選ぶ)
Kittyのキー報告を選んだイベントはKittyの形式を維持します。
[キー設定の例](../../examples/core/key_mapping.c)を`make example-key-mapping`で作ると、上キーの既定列と`55 00`へ変更した列を比較できます。

## BackspaceとDelete

両キーの動作は、`input.backspace_binding`と`input.delete_binding`で個別に指定してください。
値の接頭辞は`BOXRIG_ERASE_`です。

| 値 | Backspace | Delete |
| --- | --- | --- |
| `AUTO`、既定 | DEL、`0x7f` | `ESC [ 3 ~` |
| `ASCII_BACKSPACE` | BS、`0x08` | BS、`0x08` |
| `ASCII_DELETE` | DEL、`0x7f` | DEL、`0x7f` |
| `DELETE_SEQUENCE` | `ESC [ 3 ~` | `ESC [ 3 ~` |
| `TTY` | 通信路の消去文字。未取得ならAuto | 通信路の消去文字。未取得ならAuto |

BS／DELはCtrlで相互に反転し、AltではESCを前置します。
削除シーケンスは`CSI 3 ; modifier ~`で修飾を表し、AltのESCを重ねません。
keypad Deleteにも同じ設定を適用します。pressとrepeatを送信し、legacyのreleaseは送信しません。
Kittyが処理したイベントとapplication keypadの既存規則は、そのプロトコルに従います。

## 実行中に設定を変更する

`boxrig_terminal_config.input`は`boxrig_input_config`型です。
同じ型を`boxrig_terminal_set_input_config()`へ渡すと、Enter・両消去キー・矢印の設定を一括交換できます。
既定値は`boxrig_input_config_default()`で取得してください。

```c
boxrig_input_config input = boxrig_input_config_default();
input.enter_newline = BOXRIG_NEWLINE_LF;
input.backspace_binding = BOXRIG_ERASE_ASCII_BACKSPACE;
input.delete_binding = BOXRIG_ERASE_DELETE_SEQUENCE;
boxrig_status status = boxrig_terminal_set_input_config(terminal, &input);
```

設定交換では画面・履歴・選択・端末モードを保持します。既にキューへ入った送信列も変更しません。
不正な値、容量超過、コピー時のメモリ不足では旧設定を保持します。
`boxrig_terminal_get_input_config()`では、現在の設定と借用した矢印設定表を取得できます。
この表を使えるのは、次の交換成功か端末破棄までです。取得した設定をそのままsetterへ渡すこともできます。

UIからの変更には、フロントエンドの`set_input_config()`を使ってください。
GTK3・GTK4では`boxrig_gtkN_terminal_widget_set_input_config()`、X11・Wayland・Win32では`boxrig_PLATFORM_terminal_component_set_input_config()`です。
X11とWin32のinput adapterにも同じ設定APIがあります。
対象コンポーネントのキー押下、保留イベント、IME、貼り付けなどが残る間は`BOXRIG_UI_BUSY`となります。
既存のイベント処理を進め、入力系列が終わってから再試行してください。
各フロントエンドを所有するイベントスレッドで呼ぶ必要があります。
同じ端末を複数のコンポーネントで共有する場合は、全ての入力系列が終了したことをホスト側で確認してください。

[キー設定の例](../../examples/core/key_mapping.c)では、実行中に矢印・Enter・両消去キーを変更した送信列も確認できます。

## TTYの消去文字を使う

`BOXRIG_ERASE_TTY`は、通信路から取得した現在の消去文字を使う設定です。
取得状態は`boxrig_tty_erase.available`、値は`byte`で表します。
coreはOSへ問い合わせないため、通信路を所有するホストが`boxrig_terminal_set_tty_erase()`で渡してください。
最後に渡した状態は`boxrig_terminal_get_tty_erase()`で取得できます。
初期状態は未取得で、該当キーのAutoへ戻ります。

POSIXでは`boxrig_posix_get_tty_erase(fd, &erase)`を利用してください。
対象キーを送る直前に呼び出し、成功・失敗のどちらでも結果をcoreへ渡します。
このAPIは[tcgetattr()](https://pubs.opengroup.org/onlinepubs/9799919799/functions/tcgetattr.html)で現在の`VERASE`を取得します。
失敗時は未取得に戻り、戻り値と`errno`で理由を確認できます。
起動時の値を固定しないので、子プロセスが後から行った`stty erase`も次のキーに反映されます。
同梱のGTK3・GTK4・X11・Waylandドライバーにも、この処理を組み込んでいます。

NULと`_POSIX_VDISABLE`も、取得済みの1バイトとして送ります。
Ctrlで反転するのはBSとDELだけです。他の値はCtrlでも変えず、AltではESCを前置します。
TTY情報は設定交換や画面リセットで失われず、キーを押し続けている間も更新できます。
接続の切断・切り替え時には、ホストが`available = false`を渡し、前の通信路の値を破棄してください。

SSH・Telnet・シリアルなどでは、接続層が値を把握できる場合にだけ渡します。
Win32 PTYには取得APIがないため、未取得のままAutoを使います。
POSIXドライバーでの値の変更と受信バイトを確認するコマンドは、`make test-tty-erase`です。

## 文字コードと生バイト

既定の文字コードはUTF-8です。標準のデコーダーでは、未完の受信文字は入力終了時にU+FFFDになります。

別の文字コードを使うときは、端末生成後に`boxrig_terminal_set_encoding_hooks()`で変換器を登録してください。`BOXRIG_ENCODING_FIELDS`で変換するのは本文とタイトルで、URIや画像データは元の符号化です。ISO-2022-JPなど、制御列を含む接続全体の変換には`BOXRIG_ENCODING_STREAM`を使います。

変換器は受信・タイトル・送信の状態をアプリ側に持ちます。コールバックは `boxrig_encoding_reserve()` で出力先を確保してから状態を進め、実際に書いたバイト数を返します。`received` で制御列を含む変換後の受信バイトを観測できます。接続中に交換するときはセッションの保留入力を処理し、`boxrig_terminal_finish_input()` と `boxrig_terminal_finish_encoding_output()` を呼び、残りの出力を送ってから登録し直します。チェックポイントの保存前も同じ手順で登録を解除します。

UTF-8専用ビルドでは`CPPFLAGS=-DBOXRIG_FEATURE_ENCODING_HOOKS=0`を指定します。変換器を組み込むときは、C99の登録例、分割入力や送信待ち、接続中の交換手順を参照してください。[端末に文字コード変換器を接続する](./encoding-hooks.md)

`send_text()`には完全なUTF-8を渡します。
不正・未完の文字列は追加せずエラーを返します。
変換済みの列や独自の制御列には`send_bytes()`を使ってください。
フィールド単位の変換では、問い合わせ応答と画像のデータはそのまま送ります。ストリーム単位では、端末が生成する制御列も送信フックを通ります。

[文字コードの例](../../examples/core/encoding.c)は`make example-encoding`で作れます。
「日本語」を1バイトずつ受信し、セルと送信キューのUTF-8を確認できます。

## 容量不足と受信終了

送信の`WOULD_BLOCK`は、送信キューの空きを作れば再試行できる状態です。
`LIMIT_EXCEEDED`は一操作が上限を超えています。
改行や文字コードを変換した後の長さで判定し、一操作の一部だけを追加することはありません。

受信では`feed()`が消費したバイト数を確認し、残りを保持します。
未完の文字や制御列は次の呼び出しへ持ち越します。
受信が終わったら`finish_input()`を呼び、末尾の未完文字を処理してください。
破棄するだけでは末尾の文字は画面へ追加されません。

[セッション](./sessions.md)を使う場合は、通信路の閉鎖後に残りの受信と終了処理を進めてから`peer_closed`を通知します。

[文書目次](./index.md)
