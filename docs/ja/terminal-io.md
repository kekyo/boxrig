# 送受信する文字とキーを設定する

[English](../en/terminal-io.md)

Enterや貼り付けの改行、受信する改行、文字コードを通信相手に合わせるための設定です。
例えば、EnterでLFを送る機器との接続や、Shift_JISを使う相手との送受信に利用できます。
これらはboxrig coreの設定で、端末を生成するときに指定します。
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
完成した列へAltのESCや文字コード変換を追加しません。
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

`encoding`は既定の`BOXRIG_ENCODING_UTF8`と`BOXRIG_ENCODING_SHIFT_JIS`から選びます。
Shift_JISでは受信文字をUnicodeへ、送信するUTF-8の文字をShift_JISへ変換します。
セル、検索、選択、コピー、IMEで扱う文字列はUTF-8のままです。

Shift_JISの対応は[WHATWG Encoding Standard](https://encoding.spec.whatwg.org/#shift_jis)に従います。
不正な受信はU+FFFD、表せない送信文字はASCIIの`?`へ置換します。
別のCP932実装との完全一致や、全ての文字の往復を保証するものではありません。
対応表の[ライセンス](../../data/encoding/whatwg/LICENSE)も再配布時に添えます。

`send_text()`には完全なUTF-8を渡します。
不正・未完の文字列は追加せずエラーを返します。
変換済みの列や独自の制御列には`send_bytes()`を使ってください。
問い合わせ応答や画像のpayloadへ文字コード変換を重ねることはありません。

[文字コードの例](../../examples/core/encoding.c)は`make example-encoding`で作れます。
送信した「日本語」は`93 fa 96 7b 8c ea`となり、受信後のセルはUTF-8として確認できます。

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
