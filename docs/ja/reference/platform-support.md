# 環境別の対応と検証範囲

[English](../../en/reference/platform-support.md)

boxrig coreだけで端末出力を解釈する構成、boxrig UIで表示する構成、PTYでシェルを動かす構成には、それぞれ異なる環境条件があります。
この表では、利用する構成から必要なOSと描画・入力機能を調べられます。
機能の全体像は[対応状況](../compatibility.md)、組み込み手順は[環境別ガイド](../index.md#アプリケーションに組み込む)を参照してください。

コード上の利用条件と、その機能を確認する方法を分けて示します。
実際に表示や入力を利用できるかは、接続先の機能、OS、ドライバーにも依存します。

## core・UI・PTYの要件

| 層 | 必要なもの | 主な確認方法 |
| --- | --- | --- |
| core | C99、標準Cライブラリ。Unicode、PNG、圧縮の依存は静的ライブラリへ組み込み | portable coreテスト |
| 共通UI | core。特定のウィンドウシステムは不要 | モデル、入力、配置、時計の単体テスト |
| 環境別UI | 次節の描画・入力ライブラリ | コンポーネント、SDK、画面のテスト |
| POSIX PTY | POSIXのPTYと非ブロッキングI/O | セッションと子プロセスのテスト |
| Win32 PTY | ConPTYを提供する実行環境 | OS付属・同梱版を分けたテスト |

ビルド方法は[ビルドとインストール](../building.md)、通信路の接続は[セッション](../sessions.md)を参照してください。

## Linuxと各フロントエンド

| 構成 | ビルド・接続条件 | 主な確認方法 |
| --- | --- | --- |
| X11 | Xlib、Pango 1.50以上、Cairo 1.18以上、IBus 1.5.29以上 | Xvfbでの描画・入力・クリップボードの試験 |
| GTK 3 | GTK 3.24以上、共通のPango・Cairo・IBus | GDK X11／Wayland、Cairo／OpenGLのウィジェット・SDK試験 |
| GTK 4 | GTK 4.14以上、共通のPango・Cairo・IBus | GDK X11／Wayland、Cairo／OpenGLのウィジェット・SDK試験 |
| native Wayland | wayland-client 1.22以上、xkbcommon 1.6以上、Pango・Cairo・IBus | Westonでの描画・入力試験と、模擬サーバーでのプロトコル試験 |
| OpenGL | OpenGL 3.3 coreまたはOpenGL ES 3.0。GTKはlibepoxy、X11／WaylandはEGLなど | ソフトウェアGLでの描画試験。GPUとドライバーは利用環境で確認 |

Waylandのビルドは固定したwayland-protocols 1.48のXMLを使います。
wayland-serverはテスト用です。
xdg-shellとウィンドウ装飾、任意のlibdecor 0.2以上は、トップレベルウィンドウを持つ付属端末側で使用します。

native Waylandのコンポーネントには`wl_compositor`、`wl_shm`、`wl_subcompositor`が必要です。
生成時にない場合は利用不可、稼働中に失われた場合は致命的な接続状態として扱います。
キーボードとポインターにはseat、IMEにはtext-input-v3、コピーには対応するdata-device／primary-selectionプロトコルが別途必要です。
viewporterとfractional-scaleは組み合わせて分数倍率へ対応します。
必要なプロトコルがない構成では、その機能を利用できません。

text-input-v3のinterface version 2では文節属性を扱い、version 1では従来の未確定文字を扱います。
プロトコル定義だけを更新しても、接続先にない機能は増えません。
[プロトコル原本](https://github.com/wayland-mirror/wayland-protocols/blob/1.48/unstable/text-input/text-input-unstable-v3.xml)と[組み込み手順](../platforms/wayland.md)を参照してください。
touch、DnD、HTML・画像クリップボードには対応しません。

## Windowsの層別条件

| 機能 | コード上の条件 | 主な確認方法 |
| --- | --- | --- |
| i686のcore・共通UI・通信基礎 | Windows 2000を基準。`_WIN32_WINNT=0x0500`、PE 5.0 | 成果物のimport監査とcore・共通UI・sessionの機能試験 |
| amd64の同じ基礎部分 | Windows XP x64を基準。`_WIN32_WINNT=0x0502`、PE 5.2 | 成果物のimport監査とcore・共通UI・sessionの機能試験 |
| 標準Win32 UI | Vista SP2＋Platform Update、またはWindows 7以降のDirectWrite | Wineの機能・描画試験。実WindowsはP2試験で別途確認 |
| OpenGLの子ウィンドウ | 基本文字描画の条件に加え、OpenGL 3.3 coreまたは対応するWGL ES拡張でES 3.0 | GPU、ICD、ピクセル形式の能力を生成時に確認 |
| D3D11レンダラー | Windows 7 SP1＋Platform Update以降のDirect2D 1.1、BGRA対応device | 独立したrendererテスト |
| D3D11コンポーネント | HWND swapchainにはDXGI 1.2。alpha合成にはWindows 8以降のDirectComposition | コンポーネント・SDK試験。最終合成は実機で確認 |
| OS付属ConPTY | Windows 10 1809以降 | 動的に解決し、ない場合は利用不可 |
| system backdrop | Windows 11 build 22621以降 | 背景効果の能力を実行時に問い合わせ |

Windows 2000向けの基礎部分にnative UIとConPTYは含みません。
新しいDirectWriteのフォントfallback APIがない場合は、基本factoryとフォント一覧を使います。
新しいDPI APIにも実行時の代替経路があります。
[Windows 7 Platform Update](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/platform-update-for-windows-7)でもDirectCompositionは提供されないため、D3D11の描画と背景alphaの条件を分けています。

基礎ABIは`make test-win32-core-artifacts test-win32-ui-artifacts`で、成果物の形式やimportを監査します。
import監査とWineでの実行だけでは、各旧OS上の動作を確認したことにはなりません。
OS付属ConPTYと同梱版の選択は[配布条件](./distribution.md)を参照してください。

## 描画方式と透過・背景効果

| 構成 | 背景だけのalpha | OSのぼかしなど |
| --- | --- | --- |
| X11 | ARGB visualとcompositorが必要 | 対応するウィンドウマネージャーへの要求 |
| GTK | GDKの描画先とcompositorに依存 | 任意のprovider。GTK 4 Wayland用の接続には追加条件 |
| native Wayland | compositorがsurfaceのalphaを合成 | 対応する背景効果プロトコルが必要 |
| Win32標準描画 | alpha対応DirectWrite描画とlayered childなどの条件が必要 | DWMと対象OSの対応が必要 |
| Win32 OpenGL | WGLのピクセル形式と最終ウィンドウ合成に依存 | 同上 |
| Win32 D3D11 | composition swapchainとDirectCompositionに依存 | 同上 |

色にalphaを設定しただけでは、背後のデスクトップが見えるとは限りません。
文字を含むウィンドウ全体の透過とも別の機能です。
要求と実際の状態は[背景](../background.md)と[描画方式](../renderers.md)で説明しています。
WineでAPIが成功した結果を、Windowsの最終合成と同じとは扱いません。

## IME・通知・クリップボード・アクセシビリティ

### IMEの経路

| 経路 | 周辺文字列と編集 | 条件・制約 |
| --- | --- | --- |
| GTK 3／4 | 文書の参照と同期所有者への置換 | GTK 3のAPIは選択anchorを渡さない |
| GTK 4＋通常IBus module | 同上 | IBus 1.5.29-rc2の独自ウィジェット経路ではanchorがエンジンへ届かない |
| 直接IBus | cursorとanchorを渡し、端末内の候補を表示 | IBusへの接続が必要 |
| 直接fcitx 5 | cursorとanchor、同期置換、装飾付き未確定文字、ページ候補 | 稼働中の5系へ接続。候補通知の受信まではCLIENT表示未確認 |
| native Wayland text-input-v3 | 削除と確定を一つの置換として扱う | 接続先プロトコルが必要 |
| XIM | 対応するサーバーに文脈・文字単位の編集を提供 | サーバーが文字列変換の能力を公開しなければ利用不可 |
| Win32 | DOCUMENTFEED、選択した文字列の再変換 | 通常のIME確定はPTYへ送る。共通の同期所有者だけでは切り替わらない |

アプリケーションが文書を登録しない場合は、通常の確定文字をPTYへ送ります。
文脈だけを公開したGTK／Wayland／対応XIMでは、文書の削除を拒否し、続く確定をPTYへ流しません。
編集には同期所有者の登録が必要です。
[入力とIME](../input-ime.md)を参照してください。

GTK／Waylandへ渡す文脈にはNULを含めません。
Wayland／XIMへ渡す窓は最大4,000バイトで、選択全体が収まらなければ未対応です。
XIMのロケールで表せない要求も拒否します。
GTKの装飾はUTF-8のscalar境界を使い、不正範囲だけを除外します。
256区間を超えた装飾は省略します。
Wayland version 2は1回のdone内の異なる128範囲を上限とし、超過時も有効な文字と確定は保持します。

GTKの自動選択は、空でない`GTK_IM_MODULE`を優先します。
GTK 4 Waylandの文節表示には、runtimeのGTK 4.23.4以上とtext-input-v3 version 2の両方が必要で、それを満たさない場合は直接IBus／fcitx 5の検出経路も用意しています。
経路の選択条件は[IME選択の実装](../../../src/gtk/ime_probe.c)、文脈や編集の利用条件は[入力とIME](../input-ime.md)で確認できます。

IBus 1.5.29-rc2の通常GTK 4経路では、既定同期モードの取消通知に制約があります。
付属端末は未設定の場合だけ`IBUS_ENABLE_SYNC_MODE=2`を選びます。
ライブラリからプロセス環境は変更しません。
[IBusの同期処理](https://github.com/ibus/ibus/blob/1.5.29-rc2/src/ibusinputcontext.c#L1557)と[GTK module](https://github.com/ibus/ibus/blob/1.5.29-rc2/client/gtk2/ibusimcontext.c#L945)が出典です。

### 通知とクリップボード

全5環境の付属端末はOSC 99を通知providerへ渡します。
LinuxはGLib／GIOを用いた非同期のfreedesktop Notifications、Win32はShellの通知領域です。
既定は拒否で、許可後も初期5件、2秒ごとに1件を最大5件まで補充する制限があります。
同じ非空IDと内容の成功済み通知を1秒以内に再送すると、OSへ重複送信しません。
可視状態が不明なら表示中として扱います。

Win32通知のタイトルは最大63 UTF-16 code unit、本文は255です。
超過時はsurrogate pairを分断せず省略します。
OS設定によって表示されない場合もあります。
[NOTIFYICONDATA](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyicondataw)と[freedesktop Notifications](https://specifications.freedesktop.org/notification-spec/latest/)を参照してください。

| 構成 | 利用者のコピー・貼り付け | 付属端末のOSC 52 |
| --- | --- | --- |
| X11／GTK 3／4 | PRIMARYとCLIPBOARD | 対応する選択先への書き込み。読み取りは既定で拒否 |
| native Wayland | プロトコルと入力serialの条件下で選択を扱う | provider未接続。書き込みはUNAVAILABLE、読み取りはDENIED |
| Win32 | `CF_UNICODETEXT`。改行は必要に応じて変換 | `c/s/0`をシステムclipboardへ対応。読み取りは既定で拒否 |

UIの貼り付け上限は既定8 MiB、再試行する断片は4 KiBです。
付属端末はcoreの32 KiB／64 KiBへ明示的に合わせます。
改行をLFへそろえ、LFとタブ以外の制御文字を除去し、送信待ちになっても同じ貼り付けを継続します。
Win32では`EmptyClipboard()`成功後の`SetClipboardData()`失敗まで旧内容の復元を保証できません。
[Windowsのclipboard操作](https://learn.microsoft.com/en-us/windows/win32/dataxchg/clipboard-operations)に従う制約です。

GTK 3は`ATK_ROLE_TERMINAL`、GTK 4は`GTK_ACCESSIBLE_ROLE_TERMINAL`を設定します。
AtkTextやGtkAccessibleTextによる本文、変更、選択の公開には未対応です。
roleだけで読み上げ機能全体への対応とはしません。

## 検証方法と実環境での確認

通常テストとWindows実機のP2試験の入口は[テスト手順](../development/testing.md)を参照してください。
Xvfb、Weston、Wineや模擬コールバックによる試験では、描画、入力、APIの状態遷移を確認します。
OSのIME候補位置、デスクトップとの透過合成、GPUドライバーの挙動は、利用する実環境でも確認してください。

通常GTK 4 WaylandのOS候補位置は、[IBusの整数倍率処理](https://github.com/ibus/ibus/blob/1.5.29-rc2/client/gtk2/ibusimcontext.c#L1585)と[GNOME Shell 46の論理座標処理](https://github.com/GNOME/gnome-shell/blob/46.0/js/ui/ibusCandidatePopup.js#L211)に依存します。
分数倍率での候補位置は、端末の移動やfocusの変更も含めて確認してください。

XIMの文字列変換は、模擬コールバックの成功だけを外部サーバーとの通信成功とは扱いません。
libX11 1.8.7の返信処理にはNULL参照とコピー元の問題があります。
対象は[公式ソース](https://www.x.org/releases/individual/lib/libX11-1.8.7.tar.xz)の`modules/im/ximcp/imCallbk.c`です。
文字列変換の能力がないIBus XIM接続では、その能力を使わず入力を継続します。

## 外部仕様の出典

上記の制約のほか、[DirectWrite](https://learn.microsoft.com/en-us/windows/win32/api/dwrite/nn-dwrite-idwritefactory)、[ConPTY](https://learn.microsoft.com/en-us/windows/console/createpseudoconsole)、[DWMの背景効果](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type)、[GTKの周辺文字列](https://docs.gtk.org/gtk4/method.IMContext.set_surrounding_with_selection.html)、[Xlibの文字列変換](https://www.x.org/releases/current/doc/libX11/libX11/libX11.html#String_Conversion_Callback)を参照できます。
外部APIの存在と、boxrigでの利用条件は本表と併せて確認してください。

[ドキュメント目次](../index.md)
