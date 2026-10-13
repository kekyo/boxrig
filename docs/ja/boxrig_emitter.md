# 制御列を出力して確認する

[English](../en/boxrig_emitter.md)

`boxrig_emitter`を使うと、色やカーソル、画像、キー入力などを一つずつ試し、端末の表示や応答を確認できます。
確認用の文字、制御列、問い合わせを出力するCLIツールで、boxrig以外の端末でも実行できます。
表示結果を知りたい場合は、このページの短い実行例から始めてください。
送ったバイト列の解釈や対応範囲を調べる場合は、末尾の仕様ガイドへ進めます。

## ビルドと実行

Linux版は`make native`、Windows版は`make win32`で作成します。
ツールだけなら次の生成物を直接指定できます。

```sh
make build/boxrig_emitter/posix/boxrig_emitter
./build/boxrig_emitter/posix/boxrig_emitter --help
```

Windows版は`build\boxrig_emitter\win32\amd64\boxrig_emitter.exe`と`i686\boxrig_emitter.exe`です。
実行する端末と同じABIである必要はありませんが、OSで実行できる版を選んでください。

## 最初の確認

以下は、boxrigのX11端末でバックスペースを確認する例です。
`ABC`の末尾のCが消え、`AB`が残る結果を確認できます。
先に[ビルドガイド](./building.md#linux向けにビルドする)で`boxrig_term`も作成してください。

```sh
./build/boxrig_term/x11/boxrig_term -- \
    ./build/boxrig_emitter/posix/boxrig_emitter -t backspace
```

ケースは`ABC`を出力した後、BSと空白を使って末尾のCを消します。
`EXPECT`の説明と、`BEGIN`から`END`までの表示を比べ、`AB`が残ることを確認してください。
終了時のEnter待ちを省くには`--no-hold`を付けます。

```sh
./build/boxrig_emitter/posix/boxrig_emitter -t backspace --no-hold
```

## ケースを選ぶ

以下は、使用できるケースを一覧で確認し、Kitty graphicsの説明を読んでから、そのケースを実行するコマンドです。
対応する端末で実行すると、画像の転送・配置を使った表示を確認できます。

```sh
./build/boxrig_emitter/posix/boxrig_emitter --list
./build/boxrig_emitter/posix/boxrig_emitter --describe kitty-image-static
./build/boxrig_emitter/posix/boxrig_emitter -t kitty-image-static
```

`--list`は名前、分類、入出力の種類を列挙し、`--describe`は期待する結果と操作を表示します。
`-t`には一つの名前を指定します。
以下は、`boxrig_term`で`kitty-image-static`を表示した例です。

![Kitty graphicsの表示例](../../images/kitty-image-static.png)

## 出力と応答を確かめる

出力したという事実と、表示先が正しく解釈したという結果は分けて確認してください。

`output`は表示を観察するケース、`round-trip`は端末からの返信を読むケース、`interactive`は利用者がキーなどを入力するケースです。
`frontend`はリサイズやIMEの未確定文字など、制御列だけでは起こせない操作を試します。

`--no-hold`は末尾の待機を省きます。
問い合わせや対話操作そのものを自動化する指定ではありません。
出力だけをファイルへ保存しても、端末の返信や描画は検証できません。
既存の自動検証は[tests/test_emitter.c](../../tests/test_emitter.c)で実際の出力とcoreの結果を確認しています。

`host-effect`付きのケースは、タイトル、クリップボード、通知など画面外の状態にも関係します。
`clipboard-write`は`boxrig clipboard fixture`へ内容を変更し、`clipboard-read`は許可された場合に内容を子プロセスへ返します。
付属端末の読み取りは既定で拒否します。
通知と外部画像には、それぞれ`BOXRIG_ALLOW_NOTIFICATIONS=1`、`BOXRIG_ALLOW_GRAPHICS_EXTERNAL=1`が必要です。

許可を設定しても、接続先に機能がなければ成功しません。
例えばnative Waylandの付属端末はOSC 52 providerを接続していません。
WindowsではConPTYによる制御列の再出力も影響します。
[環境別対応](./reference/platform-support.md)を併せて確認してください。

## ケース一覧

実行ファイルの`--list`が現在の一覧です。
以下は用途別にまとめた全ケースです。

| 用途 | ケース名 |
| --- | --- |
| 通信 | `transport-bytes` |
| 文字 | `text-ascii`、`text-cjk`、`text-combining`、`text-long-grapheme`、`text-ligatures`、`text-complex-scripts`、`text-bidi`、`text-emoji`、`text-ambiguous-width`、`text-invalid-utf8` |
| C0・ESC | `backspace`、`c0-controls`、`cancel-recovery`、`esc-indexing`、`character-sets`、`terminal-reset` |
| カーソル・編集 | `cursor-movement`、`character-editing`、`line-scrolling`、`tab-stops`、`erase-display`、`erase-line`、`selective-erase`、`margins`、`save-restore-soft-reset`、`cursor-style-protection` |
| モード | `standard-modes`、`private-modes`、`alternate-screen-modes`、`title-stack` |
| 装飾・色 | `sgr-attributes`、`sgr-ansi-colors`、`sgr-extended-colors`、`sgr-underline-styles` |
| OSC | `osc-titles`、`osc-working-directory`、`osc-hyperlink`、`osc-palette`、`osc-dynamic-colors`、`clipboard-write`、`desktop-notification`、`desktop-notification-options` |
| 画像 | `kitty-image-static`、`kitty-image-external`、`kitty-animation-direct`、`kitty-animation-client`、`kitty-animation-loop`、`kitty-animation-compose` |
| 問い合わせ | `query-cursor-position`、`query-device-status`、`query-device-attributes`、`query-modes`、`query-decrqss`、`query-colors`、`clipboard-read`、`notification-query`、`query-kitty-graphics`、`query-kitty-keyboard` |
| 対話入力 | `input-key`、`input-application-cursor`、`input-application-keypad`、`input-kitty-keyboard`、`input-paste`、`input-focus`、`input-mouse`、`input-ime-commit` |
| ウィンドウ操作 | `frontend-resize`、`frontend-scrollback`、`frontend-font-zoom`、`frontend-ime-preedit` |
| 未対応からの回復 | `safe-no-op`、`unsupported-recovery` |

## 対応仕様への案内

制御列と返信の値は[端末シーケンス](./reference/terminal-sequences.md)、画像形式と上限は[画像仕様](./reference/graphics.md)、OS・IME・描画の条件は[環境別対応](./reference/platform-support.md)を参照してください。
boxrig_termの設定は[付属ターミナル](./boxrig_term.md)にあります。

[ドキュメント目次](./index.md)
