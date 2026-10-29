# ビルドとインストール

[English](../en/building.md)

ソースからboxrigをビルドし、アプリケーションで使うヘッダーとライブラリを用意する手順です。
Linux上のGCCとGNU Makeを基本とし、Windows向けはMinGW-w64でクロスビルドします。

画面を使わずに端末出力を解釈する場合や、描画を自分で実装する場合はboxrig coreを使います。
既存のウィンドウへ組み込む場合は、boxrig UIと対象環境のライブラリを加えます。
シェルと通信する構成には、さらにセッションとPTYのライブラリが必要です。
選び方は[ライブラリの構成](../../README_ja.md#ライブラリの構成)、作成後に試すプログラムは[最初の利用まで](./getting-started.md)を参照してください。

## ソースと依存を用意する

```sh
git clone --recursive https://github.com/kekyo/boxrig.git
cd boxrig
```

取得済みのソースを使う場合は、リポジトリのルートで`git submodule update --init`を実行してください。
固定版の依存ソースが`deps/`へ配置されます。
外部コードを直接修正する必要はありません。

Makefileが参照する主な開発環境は次のとおりです。
バージョン条件はこのソースツリーのビルド定義に基づきます。
使う構成に必要な開発パッケージをOSへ導入してください。

| 用途 | 必要なもの |
| --- | --- |
| 共通 | GCC、GNU Make、Git、pkg-config |
| X11 | Xlib、XComposite、XRender、XFixes、GLib／GIO |
| Linuxの文字描画 | Pango 1.50以降、Cairo 1.18以降、PangoCairo、PangoFT2、Fontconfig |
| GTK 3／4 | GTK 3.24以降／GTK 4.14以降、IBus 1.5.29以降、Wayland client。既定のGL対応構成ではlibepoxy |
| native Wayland | Wayland client／server 1.22以降、xkbcommon 1.6以降、IBus 1.5.29以降、wayland-scanner、wayland-cursor |
| OpenGL | OpenGLとEGLの開発パッケージ。Wayland版ではwayland-eglも使用 |
| Windows | amd64向けとi686向けのMinGW-w64。C++で記述したサンプル部分には対応するg++も使用 |
| Linux SDKのライセンス情報生成 | Node.js、npm、[生成ツール](../../tools/codegen/package.json)の開発依存 |
| 配布用パッケージの生成 | Bash、tar、gzip、zip。Linuxの複数ターゲット向けにはPodmanとQEMU/binfmt |

共通Makefileは、指定したターゲット以外の依存も設定の読み込み時に照会します。
不要なUIをリンクしない場合でも、pkg-configの不足メッセージが出ることがあります。
まず、導入する構成と実行したターゲットを確認してください。

coreにはUnicode処理のutf8proc、PNG画像のlibpngとzlibを同梱し、`libboxrig.a`へ組み込みます。
coreだけを使うアプリケーションには、X11やGTKをリンクする必要はありません。
UIのビルド・実行に必要なシステムライブラリは、上の表で環境ごとに確認してください。

直接fcitx 5／4の経路はGLib／GIOのD-Bus通信を使用するため、fcitx用の開発パッケージは不要です。
実行時には、利用する世代のデーモンと入力エンジンが必要です。
5.1.7、5.1.12、5.1.19と、4.2.9.9で動作を確認しています。
Debian／UbuntuでMozcを使う場合は、5系には`fcitx5`と`fcitx5-mozc`、4系には`fcitx`と`fcitx-mozc`が必要です。
4系のX11でシステム候補を表示する場合は、`fcitx-ui-classic`などのUIも導入します。
GTKへ入力を委譲する場合は、対応するGTK用IMEモジュールも導入してください。
直接接続と委譲の対応範囲は[入力とIME](./input-ime.md)を参照してください。

LinuxのSDKをインストールする前に、ライセンス表示を生成するツールを用意します。

```sh
npm ci --prefix tools/codegen
```

Wayland拡張の定義は固定した[wayland-protocols 1.48](https://gitlab.freedesktop.org/wayland/wayland-protocols/-/tags/1.48)から生成します。
生成したヘッダーやCファイルは`build/`に置かれ、公開SDKには含めません。

## Linux向けにビルドする

Linux向けのライブラリとデモをまとめて作る場合は、次を実行します。

```sh
make native
```

`make`だけを実行すると、Linux向けに加えてWindowsのamd64／i686もビルドします。
Windowsを使用しない場合は`native`を指定してください。

| 必要な成果物 | コマンド | 主な出力先 |
| --- | --- | --- |
| coreだけ | `make build/libboxrig.a` | `build/libboxrig.a` |
| GTK 3のデモ | `make boxrig-term-gtk3` | `build/boxrig_term/gtk3/boxrig_term` |
| GTK 4のデモ | `make boxrig-term-gtk4` | `build/boxrig_term/gtk4/boxrig_term` |
| native Waylandのデモ | `make boxrig-term-wayland` | `build/boxrig_term/wayland/boxrig_term` |
| X11のデモ | `make build/boxrig_term/x11/boxrig_term` | 同左のパス |

共通UI、通信、POSIX接続は、それぞれ`build/libboxrig-ui.a`、`build/libboxrig-session.a`、`build/libboxrig-posix.a`になります。
環境別の静的ライブラリも`build/`に置かれます。

### Linuxの共有ライブラリを使う

core・UI・session・POSIXの共有版は、次のコマンドで作成・インストールできます。

```sh
make shared-base BUILD_DIR=/tmp/boxrig-build
make install-shared-base BUILD_DIR=/tmp/boxrig-build PREFIX="$HOME/.local"
pkg-config --cflags --libs boxrig-ui boxrig-posix
```

生成先は`BUILD_DIR/shared/`です。指定を省略すると`build/shared/`になります。
共有版用のPICオブジェクトを使うため、通常の静的ビルドと出力先が分かれます。
インストール先には共有版と静的版の両方が入り、共有版のSONAMEは`libboxrig.so.0`などです。
配布先には、利用するライブラリの`.so.0`と、その依存となるboxrigライブラリも含めてください。
同じディレクトリの依存をロードできますが、アプリケーションからそのディレクトリを探す設定は必要です。
標準ライブラリ検索パス、アプリケーションのRUNPATH、または`LD_LIBRARY_PATH`を利用します。

`PREFIX`・`LIBDIR`・`INCLUDEDIR`・`PKGCONFIGDIR`には空白を含まない絶対パスを指定します。
ステージングには`DESTDIR`を使えます。生成したpkg-config情報はインストール先からの相対パスなので、SDK全体を移動しても利用可能です。
独自の場所へ置いた場合は、`PKG_CONFIG_PATH`へその`lib/pkgconfig`を追加してください。
静的版の依存も取得する場合は`pkg-config --static --libs`を使います。
共有版が同じ検索先にある場合、静的リンクの選択にはコンパイラ・リンカー側の設定も必要です。
公開APIはインストールしたヘッダーに宣言された関数です。`BOXRIG_PRIVATE_0`のシンボルはアプリケーションから使用しません。
同梱のlibpng・zlib・utf8procは公開されず、アプリケーションやGTKが使うcodecと独立しています。

Linux向けの全15種類を共有版で使う場合は、次を実行します。

```sh
make shared
make install-shared PREFIX="$HOME/.local"
PKG_CONFIG_PATH="$HOME/.local/lib/pkgconfig" pkg-config --cflags --libs boxrig-x11-ogl
```

`make native`単独では静的版を生成します。共有版には上記の`shared`を指定してください。
配布パッケージのLinux SDKには、次の全15種類の共有ライブラリと静的ライブラリが入ります。
表の名前には`lib`と`.so.0`が付きます。リンク時に使う`.so`も同梱します。

| 用途 | ライブラリ名 |
| --- | --- |
| 基礎 | `boxrig`、`boxrig-ui`、`boxrig-session`、`boxrig-posix` |
| GTKウィジェット | `boxrig-gtk3`、`boxrig-gtk4` |
| X11・Wayland | `boxrig-x11`、`boxrig-wayland` |
| OpenGL描画 | `boxrig-render-ogl`、`boxrig-x11-ogl`、`boxrig-wayland-ogl` |
| 通知・背景効果 | `boxrig-notification`、`boxrig-background-wayland`、`boxrig-background-gtk3`、`boxrig-background-gtk4` |

一部の構成だけを使う場合は、次のターゲットを指定します。必要なboxrigの依存ライブラリも含まれます。

| 必要な構成 | ビルド | インストール |
| --- | --- | --- |
| 基礎4種類 | `shared-base` | `install-shared-base` |
| GTK3またはGTK4 | `shared-gtk3`または`shared-gtk4` | `install-shared-gtk3`または`install-shared-gtk4` |
| X11・Wayland | `shared-native` | `install-shared-native` |
| OpenGLとX11・Wayland | `shared-opengl` | `install-shared-opengl` |
| 通知・背景効果とX11・Wayland | `shared-effects` | `install-shared-effects` |

GTKウィジェットは`GTK_OPENGL=0`でCairoだけの構成も選択できます。
生成する`.pc`のバージョンは`BOXRIG_VERSION`で指定でき、配布パッケージでは`screw-up`が取得したバージョンになります。
pkg-config名は上のライブラリ名と同じです。共有リンクには`pkg-config --cflags --libs`、静的リンクに必要な依存も取得する場合には`--static`を追加します。

## Windows向けにクロスビルドする

```sh
make win32
```

片方だけ必要な場合は`make win32-amd64`または`make win32-i686`を使います。
ライブラリは`build/win32/amd64/`または`build/win32/i686/`へ、端末デモは`build/boxrig_term/win32/<arch>/`へ生成されます。

通常のPTY接続はWindows付属のConPTYを使います。
ビルドや標準SDKのインストールに、NuGetからのConPTY取得は必要ありません。
Windows上で実行する条件は[対応状況](./compatibility.md)を確認してください。

## 配布用パッケージを生成する

Linux版は、配布先とCPUごとのPodmanコンテナで生成します。
対象は次の9種類です。Windows版は、ホストのMinGW-w64で従来どおりamd64とi686向けに生成します。

| 配布先 | リリース | CPU |
| --- | --- | --- |
| Debian | 13（trixie） | x86_64、i686、arm64、armv7l、riscv64 |
| Ubuntu | 24.04 | x86_64、arm64 |
| Ubuntu | 26.04 | x86_64、arm64 |

ホストにはPodman、GNU Make、Git、tar、Node.js 22.12以降、npmとnpxが必要です。
Node.jsは[生成ツールの依存条件](../../tools/codegen/package-lock.json)を満たす版を使用してください。
ホストと異なるCPU向けには、QEMUとbinfmtのハンドラー登録も必要です。
詳しくは[Podmanの複数アーキテクチャ向けビルド](https://docs.podman.io/en/v4.9.3/markdown/podman-build.1.html)を参照してください。
Windows版も生成する場合は、前述のクロスビルド環境を用意します。

最初に依存ライブラリを導入したイメージを作り、その後にパッケージを生成します。

```sh
./prereq.sh
./build_package.sh
```

`prereq.sh`は作成済みのイメージを再利用します。依存を更新する場合は`./prereq.sh --force`で作り直してください。
対象を絞る場合は、両方に同じ`--distro`、`--release`、`--arch`を指定します。

```sh
./prereq.sh --distro debian --arch i386,armhf,riscv64
./build_package.sh --target linux --distro debian --arch i386,armhf,riscv64 --jobs 8
```

各フィルターにはカンマ区切りで複数指定できます。
`amd64`は`x86_64`、`i386`は`i686`、`aarch64`は`arm64`、`armhf`は`armv7l`として扱います。
リリースには`13`、`noble`、`resolute`も指定でき、それぞれ`trixie`、`24.04`、`26.04`に対応します。
`--list-targets`を付けると、イメージやパッケージを作らずに対象一覧を確認できます。

`build_package.sh`はサブモジュールを初期化し、`npm ci`で生成ツールの依存を用意します。
出力先はリポジトリ直下の`artifacts/`です。既定ではLinux版9種類とWindows版2種類を生成します。
バージョン番号は、[screw-upのformatコマンド](https://github.com/kekyo/screw-up#format-command)を`npx --yes screw-up format -f -e '{version}'`で実行して取得します。

| ファイル名 | 対象 |
| --- | --- |
| `boxrig-<version>-linux-<distro>-<release>-<arch>.tar.gz` | 指定した配布先とCPU向けのLinux SDK |
| `boxrig-<version>-win32-amd64.zip` | 64ビットWindows |
| `boxrig-<version>-win32-i686.zip` | 32ビットWindows |

例えばUbuntu 24.04のx86_64版は`boxrig-0.1.0-linux-ubuntu-24.04-x86_64.tar.gz`です。
展開先ディレクトリ名にも同じバージョン番号が入ります。
Linux版は、ファイル名に記された配布先とCPUに合わせて選んでください。

各アーカイブには、対象OSのSDK、端末デモ、emitter、英語のREADME、ライセンス文書が入ります。
利用例のソースコードは同梱しません。リポジトリの[examples/](../../examples/)を参照してください。
日本語文書は同梱せず、READMEからWeb上の日本語版を参照できます。
Linuxの端末デモは`bin/boxrig_term_x11`、`boxrig_term_gtk3`、`boxrig_term_gtk4`、`boxrig_term_wayland`です。
Windows版には`bin/boxrig_term.exe`とlauncherを収録します。

各ターゲットは順に処理します。`--jobs`は一つのターゲット内のMakeの並列数で、既定は`JOBS`環境変数、未指定ならホストのCPU数です。
選択した全ターゲットが成功してから、同じ名前のアーカイブを置き換えます。
コンテナ用イメージが未作成の場合は、必要な`prereq.sh`のコマンドを表示して終了します。

Windows版だけを生成する場合は`./build_package.sh --target win32`を使います。
ホストの開発環境でLinux版を生成する場合は`./build_package.sh --target native`を使います。
この場合の名前は`boxrig-<version>-linux-<arch>.tar.gz`で、`<arch>`は`uname -m`の結果です。

ビルドを伴わないスクリプト検証は`bash tests/check_package_scripts.sh`、全体テストは`make -j1 test`で実行します。
全体テストのパッケージ検証はホストLinux版とWindows版が対象です。

## 必要なモジュールをインストールする

`PREFIX`でインストール先を指定します。
システムへ書き込まずに試す場合は、リポジトリの`build/`配下を指定できます。

```sh
make install-gtk4 PREFIX="$PWD/build/sdk-gtk4"
```

| 使用する構成 | インストールターゲット | pkg-config名 |
| --- | --- | --- |
| GTK 3 | `install-gtk3` | `boxrig-gtk3` |
| GTK 4 | `install-gtk4` | `boxrig-gtk4` |
| native Wayland | `install-wayland` | `boxrig-wayland` |
| OpenGL描画API | `install-render-ogl` | `boxrig-render-ogl` |
| X11 OpenGLコンポーネント | `install-x11-opengl` | `boxrig-x11-ogl` |
| Wayland OpenGLコンポーネント | `install-wayland-opengl` | `boxrig-wayland-ogl` |
| Win32 core | `install-core-win32-amd64` | `boxrig` |
| Win32標準UI | `install-win32-amd64` | `boxrig-win32` |
| Win32 OpenGL | `install-win32-opengl-amd64` | `boxrig-render-ogl`、`boxrig-win32-ogl` |
| Direct3D 11描画API | `install-render-d3d11-amd64` | `boxrig-render-d3d11` |
| Direct3D 11コンポーネント | `install-win32-d3d11-amd64` | `boxrig-win32-d3d11` |

Windowsのi686版はターゲット末尾を`i686`に置き換えます。
異なるアーキテクチャには別のインストール先を指定してください。

通知とGTK・Waylandの背景効果には、別のインストールターゲットもあります。
使うときに[背景のガイド](./background.md)と公開ヘッダーを確認してください。
インストールターゲットがない単独構成は、ソースツリーで生成したライブラリを使用します。
対象OSのSDKをまとめて用意する場合は、上記の`build_package.sh`を使います。

## インストール先からリンクする

公開ヘッダーは`include/boxrig/`、ライブラリは`lib/`、pkg-config情報は`lib/pkgconfig/`に配置されます。

```sh
export PKG_CONFIG_PATH="$PWD/build/sdk-gtk4/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
pkg-config --cflags --libs --static boxrig-gtk4
```

上記の個別インストールターゲットは静的アーカイブを配置します。
`--static`を付けると、アーカイブに必要な依存ライブラリも取得できます。Linuxで共有版も使う場合は`install-shared`を選んでください。
コマンドに表示されるシステムライブラリの開発パッケージも必要です。
Ubuntu 24.04でGTKなどの静的依存をリンクする場合は、D-Busが指定する`libsystemd`を解決するため、[`libsystemd-dev`](https://packages.ubuntu.com/noble/libsystemd-dev)も導入してください。

Windows向けはホスト側のライブラリを混ぜないよう、対象のインストール先だけを指定します。

```sh
make install-win32-amd64 PREFIX="$PWD/build/sdk-win64"
PKG_CONFIG_LIBDIR="$PWD/build/sdk-win64/lib/pkgconfig" \
  pkg-config --cflags --libs --static boxrig-win32
```

`make install-*`でインストールする利用例はモジュールごとに異なります。
`share/boxrig/examples/`には、Win32の実行可能な例や各環境の外部スクロールバー用ヘッダーなどを配置します。
全てのデモがインストールされるわけではありません。

Win32のUIを静的リンクする場合、boxrig由来の追加DLLは不要です。
OS付属ConPTYにも追加のDLLやEXEは不要ですが、任意のboxrig DLLを選んだ場合はそのDLLを同梱します。
同梱版ConPTYを明示して使用する場合は、対象アーキテクチャのDLL、OSに合うホスト、ライセンス表示が必要です。
[ConPTYの配布ファイル](../../deps/conpty/README.md)を確認してください。

## 任意機能を選ぶ

GTKの既定ビルドはCairoとOpenGLの両方を含みます。
Cairoだけの構成は次のように作ります。

```sh
make GTK_OPENGL=0 boxrig-term-gtk3 boxrig-term-gtk4
```

再び`GTK_OPENGL=1`を指定するとGL対応構成へ戻ります。
ライブラリの設定の既定はCairoですが、付属デモはGLを優先します。
ビルドに組み込む方式と、実行時に選ぶ方式を区別してください。

native Waylandのデモは、既定の`WAYLAND_LIBDECOR=auto`でlibdecor 0.2以降を検出します。
`1`は導入を必須とし、`0`は使用しません。
libdecorはデモの装飾に使う依存で、通常のWaylandコンポーネントSDKには含めません。

## ビルドに失敗したとき

pkg-configのエラーは、指定された開発パッケージの有無とバージョンを調べます。
Windowsのコンパイラがない場合はMinGW-w64を導入するか、Linuxのターゲットだけを指定してください。

ビルドは警告をエラーとして扱います。
警告を無効にして進める前に、使用しているコンパイラ、依存の版、実行したコマンドを確認します。
ソースツリーのファイルと別のSDKのヘッダーを混ぜないことも確認してください。

[文書目次へ戻る](./index.md)
