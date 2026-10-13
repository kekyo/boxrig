# SDKを使ったアプリケーションの配布

[English](../../en/reference/distribution.md)

boxrigを組み込んだアプリケーションの配布物は、coreだけを使うか、UIやPTYを加えるかで変わります。
このページでは、現在用意できるSDKを示した後、リンク時と実行時の依存、OS別の同梱物、ライセンスを説明します。
SDKの作成手順は[ビルドとインストール](../building.md)、最初の利用例は[SDK向けREADME](../../../README_pack.md)を参照してください。

## 現在提供するSDKとモジュール

SDKは、ヘッダー、ライブラリ、pkg-config定義、利用例、ライセンス資料を任意のprefixへインストールする形式です。
全機能を含む共通アーカイブをダウンロードできるという説明ではありません。

| 構成 | インストール対象・pkg-config名 |
| --- | --- |
| GTK 3／4 | `install-gtk3`／`install-gtk4`、`boxrig-gtk3`／`boxrig-gtk4` |
| native Wayland | `install-wayland`、`boxrig-wayland` |
| 共通OpenGL renderer | `install-render-ogl`、`boxrig-render-ogl` |
| X11／WaylandのOpenGL component | `install-x11-opengl`／`install-wayland-opengl`、`boxrig-x11-ogl`／`boxrig-wayland-ogl` |
| Windowsのcore、標準component | `install-core-win32-{amd64,i686}`、`install-win32-{amd64,i686}`。`boxrig`、`boxrig-win32` |
| WindowsのOpenGL | `install-win32-opengl-{amd64,i686}`。`boxrig-render-ogl`、`boxrig-win32-ogl` |
| WindowsのD3D11 | `install-render-d3d11-{amd64,i686}`、`install-win32-d3d11-{amd64,i686}`。`boxrig-render-d3d11`、`boxrig-win32-d3d11` |

標準は静的ライブラリです。
WindowsのOpenGLにはDLLとimport libraryの構成もあります。
D3D11の現在のインストールは静的ライブラリです。
対象ABIごとにprefixを分けてください。

## リンク時と実行時の依存

ヘッダーは`include/boxrig/`、ライブラリと`.pc`は`lib/`と`lib/pkgconfig/`、利用例は`share/boxrig/examples/`、ライセンス資料は`share/licenses/boxrig/`へ入ります。
モジュールによって含む利用例が異なります。

`.pc`は自身の位置からprefixを求めるため、ディレクトリごと移動できます。
現状の`Version`は`0.0.0`です。
Windows向けのリンクでは`PKG_CONFIG_LIBDIR`を対象SDKだけに設定し、ホストLinuxの`.pc`を混ぜないでください。
静的リンクの追加依存は`pkg-config --static --libs`で取得します。

静的ライブラリをリンクしても、GTK、Pango、Cairoなどのシステム共有ライブラリまで実行ファイルへ入るわけではありません。
必要な実行時依存は選んだ構成で変わります。

coreがウィンドウシステムに依存しないことと、第三者コードを含まないことは別です。
utf8proc、libpng、zlibはcoreへ組み込まれるため、coreだけを配布する場合も[第三者ライセンス](#第三者ライセンスと出典)を確認してください。

## Linuxで配布する

coreのUnicode、PNG、圧縮の依存は`libboxrig.a`に含まれます。
UIを使うアプリケーションには、選んだX11、GTK、Wayland、Pango、Cairo、IBusなどの共有ライブラリが必要です。
OpenGLには対象環境のEGL／GLとドライバーも必要です。

付属端末のURL起動にはcardioを使いますが、組み込みアプリケーションは自身のホスト処理を選べます。
付属端末の全依存を、coreだけのアプリへ課す必要はありません。

配布時のライセンスの扱いは、監査済みの[第三者ライセンス資料](../../../packaging/licenses/README_ja.md)に従って、実際に使う構成を確認してください。
システム共有ライブラリの利用と、そのライブラリ自体の同梱・静的リンクでは確認する条件が異なります。
[LGPL 2.1原文](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html)も参照できます。

## Windowsで配布する

標準の静的リンクではboxrig自身のDLLは不要です。
OpenGLのDLL構成を選んだ場合は、リンクしたDLLと必要なランタイムを対応するABIで同梱します。
実行ファイルが追加した依存も含めてimportを確認してください。

付属端末は`boxrig_term.exe`と同じ場所の`boxrig_term_launcher.exe`を使います。
このlauncherはデモの子プロセス起動用で、SDK利用アプリケーション全般の必須ファイルではありません。

DirectWrite、DWM、Direct3D、OpenGLはOSやドライバーが提供します。
boxrigのファイルだけを置いて、古いOSへこれらの機能を追加できるわけではありません。
背景透過やGPU描画に必要な条件は[環境別対応](./platform-support.md)へ分けています。

## OS付属ConPTYと同梱版を選ぶ

既定は`BOXRIG_WIN32_PTY_RUNTIME_SYSTEM`で、OS付属ConPTYを動的に読み込みます。
この構成ではMicrosoftのDLLや実行ファイルを同梱しません。

| 選択 | 動作 |
| --- | --- |
| SYSTEM | OS付属版を使う |
| PACKAGED_PREFERRED | 指定ディレクトリの同梱版を優先。対象ファイルや機能が利用できない条件でOS付属版へ戻る |
| PACKAGED_REQUIRED | 同梱版だけを使い、失敗時はOS付属版へ戻らない |

PREFERREDは全ての失敗を隠す設定ではありません。
アクセス権、資源不足などの失敗や、開始済みの子プロセスをやり直すための切り替えは行いません。
診断で示すhost候補の情報も、実際に起動したプロセスの証明とは区別します。
設定の定義は[pty.h](../../../include/boxrig/win32/pty.h)を参照してください。

任意の`make conpty-runtime-assets`は、Makefileで固定したConPTY 1.24.260710001を取得し、SHA-256を照合します。
`conpty.dll`はアプリケーションのABI、`OpenConsole.exe`はOSのアーキテクチャに合わせます。
配布時は必要なx86／x64／arm64のhost配置を保ち、`LICENSE.conpty.txt`と`NOTICE.conpty.md`も含めてください。
[対象パッケージ](https://www.nuget.org/packages/Microsoft.Windows.Console.ConPTY/1.24.260710001)と[対応する上流リリース](https://github.com/microsoft/terminal/releases/tag/v1.24.11911.0)が出典です。

## 第三者ライセンスと出典

boxrigは[MIT License](../../../LICENSE)です。
第三者コードとデータの条件は、それぞれの原文を保持します。
SDKの`share/licenses/boxrig/`にはutf8proc、libpng、zlib、Unicode、WHATWG Encoding、構成別のWayland、cardio、MinGW-w64、GCCなどの資料が入ります。

取得元URLとハッシュは[sources.json](../../../packaging/licenses/sources.json)、構成ごとの説明は[第三者ライセンス資料](../../../packaging/licenses/README_ja.md)にあります。
GCCのランタイムには[GCC Runtime Library Exception](https://www.gnu.org/licenses/gcc-exception-3.1.html)も関係します。
別の版のツールチェーンで配布する場合は、その版の原文と照合してください。

## 未提供の配布機能

このガイドの配布物は、ソースからMakefileのinstallターゲットで生成するSDKです。
リリースアーカイブの自動作成と、公開ABIの互換性を管理する仕組みは提供していません。

[ドキュメント目次](../index.md)
