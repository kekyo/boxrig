# 開発時に検証する

[English](../../en/development/testing.md)

boxrigのコードや公開例を変更した人向けに、解析結果、画面表示、入力、SDKからの利用を確認する手順を説明します。
coreの機能、UIの表示や操作、ターミナル実装例への接続を、それぞれのテストで確かめます。
ライブラリを利用するアプリケーションの導入は[ビルドガイド](../building.md)を参照してください。

## テスト環境を用意する

C99のGCC、Make、LinuxのGUI開発パッケージ、MinGWのamd64／i686、Wine、Node.jsとnpmを用意します。
GUIの自動試験にはXvfb、Weston、IBusなど、対象に応じた実行環境も必要です。
GTKのAT-SPI試験をビルドするには、`pkg-config`で`atspi-2`と`gobject-2.0`を取得できる開発パッケージを用意します。AT-SPI・Orca試験を実行するには、D-Busのアクセシビリティサービス、Xvfb、Orca、`xdotool`も必要です。パッケージ名は利用するOSの配布物に従って確認してください。
具体的なビルド依存は[ビルドとインストール](../building.md)、npm依存は[生成ツールのpackage.json](../../../tools/codegen/package.json)と[Win32 P2のpackage.json](../../../tests/p2/win32/package.json)で確認できます。

```sh
npm ci --prefix tools/codegen
npm ci --prefix tests/p2/win32
```

必要なパッケージがなければ、名前と実行できない範囲を記録して導入します。
環境がないことをテスト成功にはしません。

## 全体テストを実行する

リポジトリのルートで実行します。

```sh
make -j1 test
```

core、UI、描画、各環境のコンポーネント、SDK、公開例、生成ツールなどを含むため時間がかかります。
nativeの対象ではASan／UBSanも使い、Win32は両ABIをWineで確認する対象があります。
GUIやWineの共有状態を避けるため、まず逐次実行を使ってください。

Windows実機のP2試験は、[実機の準備](#windows実機を準備する)を済ませてから`make test-p2-win32`で別途実行します。

アクセシビリティバスを持たない隔離環境では、支援技術への接続を必要としないGTK試験に限り、`NO_AT_BRIDGE=1 GTK_A11Y=none`を指定できます。
`test-gtk3-atspi`、`test-gtk4-atspi`、`test-gtk3-orca`、`test-gtk4-orca`は起動時にこれらの変数を解除し、D-Busのアクセシビリティサービスへ接続します。

AT-SPIだけを確認するには、次を実行します。Orca試験は、AT-SPIに加えてスクリーンリーダーが端末本文を読み上げることを確認します。

```sh
make test-gtk3-atspi test-gtk4-atspi
make test-gtk3-orca test-gtk4-orca
```

## 機能別に実行する

修正中は関係するテストで問題を再現し、修正後に同じ条件で確認します。
最後は全体テストを実行してください。

| 対象 | 主なtarget |
| --- | --- |
| 改行・キー・文字コード | `test-terminal-newline`、`test-terminal-key-mapping`、`test-terminal-codec` |
| 点滅・装飾 | `test-text-blink`、`test-cursor-style`、`test-selection-style` |
| 背景・通知 | `test-background-layers`、`test-notification-dispatch`、`test-notification-samples` |
| GTKとIME | `test-ime-clause`、`test-gtk-ime-routing`、`test-gtk-ibus-direct` |
| fcitx 5 | `test-fcitx5`、`test-fcitx5-panels`、`test-fcitx5-auto` |
| fcitx 4 | `test-fcitx4`、`test-fcitx4-panels-core`、`test-fcitx4-panels-gtk`、`test-fcitx4-discovery` |
| Wayland入力 | `test-wayland-input`、`test-wayland-candidates-startup` |
| ビルド時の生成処理 | `test-codegen` |
| Win32の初回描画の同期 | `test-win32-first-frame-fence`、`test-win32-sample-first-frame-fence` |

targetの依存と実行条件は[Makefile](../../../Makefile)が定義しています。
実行ファイルのコンパイルだけで終わるtargetと、起動まで行うtargetを区別してください。

実デーモンの試験では、Mozcと、試験専用のD-Bus・表示サーバー・設定ディレクトリーを使います。
4系と5系を分けたPodman環境で、Debian 13、Ubuntu 24.04、Ubuntu 26.04の収録パッケージを確認できます。
ホストのIME設定やユーザー辞書は共有しません。

```sh
bash tests/ime/run_distribution.sh debian13 fcitx5
bash tests/ime/run_distribution.sh debian13 fcitx4
```

第1引数には`ubuntu2404`と`ubuntu2604`も指定できます。
結果は`build/test-results/ime-distributions/`にあり、パッケージ版、入力試験のログ、付属ターミナルの画像・動画を保存します。
4系は実際の4.2.9.9で検証し、5系の試験では4系互換サービスとAUTOの選択も確認します。

## SDKと文書中の利用例を確認する

以下は、インストール済みSDKから利用例をビルドする試験と、step-by-stepのコードを取り出して実行する試験です。
READMEの短い抜粋に対応する全体例を、公開ヘッダーとライブラリだけで利用できることを確かめます。

```sh
make test-gtk3-package test-gtk4-package test-wayland-package
make test-opengl-packages test-win32-sdk
make test-simple-ui-guide
```

SDK試験は、一時的なprefixへインストールした公開ヘッダー、ライブラリ、pkg-configから利用例を作ります。
ソースツリーの私有ヘッダーが偶然見える条件で成功しただけでは不十分です。

UIガイドは、既存の抽出器がMarkdown内のコードを取り出し、ビルドと画面・入力の確認まで行います。
[抽出器](../../../tests/test_ui_guide.c)と[ガイドのMakefile](../step-by-step/Makefile)が接続点です。
文書だけの変更でも、抽出されるコードやビルド入力を変えた場合はこの接続を確認します。

## GUI・動画・実機を確認する

画面試験は期待する文字、色、画像、選択、座標を実際に描画して確認します。
マスター画像を新設・変更する場合は、人が画像を見て意図した内容か確かめてください。

点滅、IMEの確定と取消、候補の移動などは時間方向の挙動も対象です。
既存の動画試験ではframeの順序と変化を確認します。
静止画1枚を、ちらつきや重複確定がない証拠にはしません。

## Windows実機を準備する

Windows 11のamd64 PCと、試験で使うGPU・モニターを用意します。
画面・入力の試験では、利用者がローカルコンソールへサインインし、デスクトップのロックを解除しておきます。
agentはその対話デスクトップで起動してください。
物理画面の確認には、WindowsサービスやRDP専用セッションを使いません。

agent-roverはcontrollerと実機で同じ版を使います。
現在の固定版は[Win32 P2のpackage.json](../../../tests/p2/win32/package.json)にある0.10.0、既定の接続セクションは`win11`です。
バージョン管理外の`.agent-rover`へ接続先を設定します。

```ini
[win11]
windows-test.example.invalid=<access-token>
```

SDKは作業ディレクトリを先に探し、ファイルや対象セクションがなければホームディレクトリを調べます。
npmで実行するP2の作業ディレクトリは`tests/p2/win32/`です。
接続設定の詳細は[agent-roverの公式説明](https://github.com/kekyo/agent-rover/blob/0.10.0/README_ja.md#接続設定ファイル)を参照してください。
既定portは39397で、通信本文は暗号化されません。
接続元を制限したLANやVPNなど、[上流の通信条件](https://github.com/kekyo/agent-rover/blob/0.10.0/README_ja.md#特徴)に合う環境を使い、tokenをログや画面記録へ含めないでください。

OS build、GPU・driver、画面の倍率、使用するfontとIMEを記録し、対象ウィンドウが前景にあることと実際の入力先を確認します。
準備を終えたら、リポジトリのルートで実行します。

```sh
make test-p2-win32
```

実機が使用中なら接続を待ちます。
試験本体の終了後は、所有するprocessと一時領域を回収し、変更した画面・focus・pointer・設定を戻して接続を解放します。
切断や画面のロックで確認できなかった項目は、その理由とともに記録してください。
Wine、実Windows、GPU実機の結果は分けて記録してください。

## 失敗と未実施を記録する

結果には対象コミット、コマンド、環境、終了コード、ログ、PASS／FAIL／SKIPと理由を残します。
初回失敗と再実行成功は両方を残し、再実行だけで不安定な挙動を消したことにはしません。

コンパイラー警告、実行時の環境警告、故障注入で期待する診断を分けて調べます。
失敗が計画の変更に由来するか、既存実装や実行環境の問題かを確かめ、必要なら計画へ戻ります。
環境を模擬した試験の成功から、未接続の外部サービスや未使用OSでの成功を推定しません。

[ドキュメント目次](../index.md)
