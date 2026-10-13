# 最初の利用まで

[English](../en/getting-started.md)

boxrig UIでシェルを表示する例と、boxrig coreで文字と色を読み取る例を試します。
先に[READMEの機能と表示例](../../README_ja.md#機能)を見て、作りたいアプリケーションに近い構成を選んでください。
C99のプログラムをビルドできることを前提にしています。

## 作るものを選ぶ

ウィンドウに端末を埋め込みたい場合は、boxrig UIから始めてください。
フォントや入力処理を備えたコンポーネントを、X11、GTK 3／4、native Wayland、Win32のアプリケーションへ配置できます。

独自の描画環境や、画面を持たない解析処理にはboxrig coreを使います。
端末へ流れたバイト列を解釈し、文字、色、カーソルなどの状態を取得できます。

| 試す構成 | このページで確認する結果 | 実装を学ぶガイド |
| --- | --- | --- |
| boxrig UI | X11ウィンドウでシェルを操作する | [UI向けstep-by-step](./step-by-step/step-by-step-ui.md) |
| boxrig core | ウィンドウを開かず、`H color=1`を出力する | [core向けstep-by-step](./step-by-step/step-by-step-core.md) |

シェルを操作するには、表示に加えてPTYなどの通信路が必要です。
coreの生成と子プロセスの起動は別の操作です。
[構成要素の役割](./architecture.md)も参照してください。

## 必要な環境とSDKを用意する

ソースツリーから試す場合は、Git、GCC、GNU Make、pkg-configを用意し、[ビルド手順](./building.md)に従って依存ソースを取得します。
以降のコマンドはリポジトリのルートで実行します。

```sh
git submodule update --init
make build/libboxrig.a
```

これでcoreの静的ライブラリを用意できます。
端末解析にX11やGTKをリンクする必要はありません。
ただし、現在の共通Makefileは設定を読む際に各UI依存をpkg-configで調べます。
依存不足のメッセージが出た場合は、[ビルド環境の表](./building.md#ソースと依存を用意する)で確認してください。

SDKをインストール済みの場合は、そのSDKの公開ヘッダーとライブラリを使えます。
現在提供するインストール先とpkg-config名は、[インストールの一覧](./building.md#必要なモジュールをインストールする)を参照してください。
ソースツリーを使わずにSDKから試す場合は、[SDK向けREADMEの利用例](../../README_pack.md#Your first example)へ進んでください。
以下では、ソースツリーで用意したライブラリと実行例を使います。

## 最小の利用例を実行する

### UIでシェルを表示する

X11が使える環境で次を実行すると、シェルを操作できる端末ウィンドウが開きます。
UI向けstep-by-stepの全段階をまとめた実行例です。

```sh
make -C docs/ja/step-by-step
./docs/ja/step-by-step/simple_ui_term
```

`simple_ui_term`は、boxrig UIによる表示と入力に、シェルの起動とPTY通信を組み合わせています。
最初は固定文字列を表示するプログラムから作りたい場合は、[最初のバージョンの実装](./step-by-step/step-by-step-ui.md#最初のバージョンの実装)へ進んでください。
X11以外の環境で表示を試す場合は、[boxrig_termの起動方法](./boxrig_term.md#各環境で起動する)から選べます。

### coreで文字と色を取得する

画面を開かずに、色指定を含む端末出力を解釈する例です。
[READMEのcoreの例](../../README_ja.md#画面を使わずに端末出力を解釈する)は、関数本体からの抜粋です。
`hello_core.c`に`#include <boxrig/terminal.h>`と`#include <stdio.h>`を記述し、`int main(void) { ... }`の本体へ例のコードを置きます。
`include/`と`build/libboxrig.a`を使い、次のようにビルドします。

```sh
cc -std=c99 -Wall -Wextra -Wpedantic -Werror \
  -Iinclude hello_core.c build/libboxrig.a -o hello_core
./hello_core
```

実行結果は次のとおりです。

```text
H color=1
```

この例は、赤字指定を含む`Hello`を端末に投入し、左上のセルを読み取っています。
制御シーケンス自体はセルに現れず、文字の`H`とパレットの色番号`1`として取得できます。

## 実行結果を確かめる

coreの例は終了コード0で成功を返します。
色番号と文字を実際に読み取って判定するため、コンパイルできたことだけを成功条件にはしていません。

UIの例が画面を開けない場合は、X11接続が利用できること、必要なフォントがあることを確認します。
Wayland上ではXWaylandを使う方法もありますが、native Waylandの組み込み方法とは異なります。
対象環境がX11以外なら、[付属デモの起動方法](./boxrig_term.md)で使用するフロントエンドを選んでください。

## 次のガイドを選ぶ

| 次に行うこと | 読む文書 |
| --- | --- |
| 小さな端末を段階的に作る | [UI向けstep-by-step](./step-by-step/step-by-step-ui.md) |
| 自分でセルを描画する | [core向けstep-by-step](./step-by-step/step-by-step-core.md) |
| どの処理をアプリケーションで実装するか知る | [基本概念](./architecture.md) |
| ビルド構成やインストール先を変える | [ビルドとインストール](./building.md) |
| 特定の機能を使う | [文書目次](./index.md) |

[文書目次へ戻る](./index.md)
