# 入力とIMEを接続する

[English](../en/input-ime.md)

boxrig UIの標準コンポーネントを使うと、キーやマウスで端末を操作し、IMEの未確定文字や候補を表示できます。
OSからの入力受付はコンポーネントが担当し、boxrig coreが入力を端末のモードに合う送信列へ変換します。
アプリケーションは、セッションなどでその送信列を通信相手へ渡します。

独自の入力経路を作る場合は、通常のキー入力、確定した文字列、貼り付けを区別して接続します。
通常の端末入力を説明した後、アプリケーションが本文を所有する編集文書への接続を説明します。

## キー・マウス・貼り付けを受け取る

独自に受け取ったキーは`boxrig_key_event`、マウスは`boxrig_mouse_event`としてcoreへ渡します。
以下は、確定済みの「日本語」を、作成済みの端末の送信キューへ追加する関数です。
独自の入力受付から呼ぶ例であり、標準コンポーネントがすでに処理した入力へ重ねて呼ぶ必要はありません。

```c
#include <boxrig/terminal.h>

boxrig_status commit_text(boxrig_terminal *terminal)
{
    const char text[] = "日本語";
    return boxrig_terminal_send_text(terminal, text, sizeof(text) - 1);
}
```

成功すると送信キューへ追加されます。
画面へ直接「日本語」が書かれるわけではなく、通信先が受け取って応答した内容が画面へ反映されます。
`WOULD_BLOCK`なら送信を進めた後、同じ確定入力を一度だけ再試行してください。

貼り付けはBEGIN、CHUNK、ENDの段階を使います。
各chunkは完全なUTF-8文字を含む必要があります。
ネイティブのコピー・貼り付けと、送受信の改行設定は[送受信設定](./terminal-io.md)にまとめています。

## 未確定文字と候補を表示する

未確定文字はcompositionとして表示へ重ねます。
端末のセルへ書き込むと、通信先からの出力と区別できなくなるためです。
標準コンポーネントはIMEに合わせてカーソルの表示と候補位置も調整します。
fcitx 4の端末内候補では、未確定文字も候補と同じパネルへ表示します。

候補表示の方針はAUTO、SYSTEM、CLIENTです。
CLIENTを希望しても、入力経路が候補を提供できなければアプリ内表示は利用できません。
希望した設定と実際のcandidate owner、理由を診断から確認します。

## 入力経路を選ぶ

| 環境 | 主な入力経路 | 確認する情報 |
| --- | --- | --- |
| native X11 | XIM | 接続状態、入力所有者、候補の所有者 |
| GTK 3／X11、GTK 3／4／Wayland | GtkIMContext、直接IBus、直接fcitx 5／4 | `get_ime_state()`のroute、reason、direct |
| GTK 4／X11 | GtkIMContext | 選択されたGTKモジュール |
| native Wayland | コンポジターのtext-input、直接IBus、直接fcitx 5／4 | contextの入力経路とdirect、コンポーネントの候補診断 |
| Win32 | IMM、編集文書向けのTSF接続 | 入力能力、再変換や編集の可否 |

GTKとnative Waylandの既定方針はAUTOです。GTKでは、明示した`GTK_IM_MODULE`と、GTKが選択済みのfcitxモジュールを優先し、GTKへ入力を委譲します。GTKが処理できるWayland環境も、委譲の対象です。直接接続が必要な場合に、IBusとfcitx 5／4を検出します。native X11は引き続きXIMを使用します。

native WaylandのAUTOは、候補方針がCLIENT以外でtext-input-v3 version 2が利用できる場合に、コンポジター経由の入力を優先します。CLIENTを明示した場合は直接接続を探索します。入力経路をWAYLANDに固定した場合は、その指定を維持します。

直接接続のAUTOでは、そのホストに適用されるGTKモジュール、`XMODIFIERS`、明示アドレスの順に選択の手掛かりを使います。明示アドレスはIBusの`IBUS_ADDRESS`と4系の`FCITX_DBUS_ADDRESS`です。native Waylandは`GTK_IM_MODULE`を参照しません。`fcitx`というモジュール名や`@im=fcitx`は4系と5系の両方を表すため、稼働中のサービスを調べて世代を選びます。

手掛かりがなく複数の独立したサービスが動いている場合、初回の優先順位はfcitx 5、IBus、fcitx 4です。同じ提供元の互換サービスは重複として数えず、5系の直接接続を優先します。選択後に別のサービスが現れても、接続中の方式を維持します。検出のためにデーモンを起動することはありません。

アプリケーションはGTKの`BOXRIG_GTK_IME_FCITX5`／`BOXRIG_GTK_IME_FCITX4`、native Waylandの`BOXRIG_WAYLAND_IME_FCITX5`／`BOXRIG_WAYLAND_IME_FCITX4`で世代を明示できます。IBusも明示指定の対象です。指定した直接接続先が利用できない場合、別の直接接続先へ切り替えず、フレームワークの入力経路を使います。GTK 4／X11の明示指定も、GTKの対応モジュールへの委譲です。描画方式とIME方針は別々に選びます。

fcitx 4は`DISPLAY`の表示番号に対応する接続先を使います。`DISPLAY`がない場合は0です。`FCITX_DBUS_ADDRESS`を設定すると、そのアドレスを優先し、不正または接続不能でも別アドレスへ切り替えません。指定がなければfcitx 4が保存した接続情報とセッションバスを調べます。別のログインセッションの環境変数を持ち込まないでください。[fcitx 4の接続情報](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/lib/fcitx-gclient/fcitxconnection.c)

診断の`direct`には、選択した方式、接続状態、選択理由、設定の競合、複数候補、互換サービスの有無と、要求・エラー・コンテキストの件数が入ります。`CONNECTING`は検出または接続中、`UNAVAILABLE`は対象サービスが利用できない状態です。`DISABLED`ならフレームワークへ委譲しています。`READY`でも各入力先のコンテキスト生成が完了するまでは入力が待機する場合があります。設定名だけで使用中の経路を判断せず、routeと`direct.backend`を確認してください。

fcitx 5のクライアント候補は、受信したページの候補と上下の補助文字列を表示します。ページ内の候補数・選択位置と前後ページの有無を扱い、総候補数や絶対ページ番号は推測しません。

fcitx 4では、未確定文字とカーソル、候補、上下の補助情報を一つの文字列パネルへ表示します。候補をキーボードで選択し、ページ移動、確定、取消を操作できます。通知に含まれない個別候補の選択強調や文節装飾は再現しません。候補の文字列は分割せず表示し、診断の候補数は0です。独自の描画先は`boxrig_ui_candidate_snapshot`の`STRING_PANEL`形式と`panel_text`、`panel_preedit`、`panel_cursor`を使用します。カーソル位置はUTF-8のバイト単位です。[4系の候補通知](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/lib/fcitx-gclient/fcitxclient.c#L1097)、[候補API](../../include/boxrig/ui/candidates.h)

4系のWayland直接接続では、候補のAUTOは描画可能なクライアントパネルを選びます。SYSTEMを明示した場合の位置はシステム側のUIに依存し、全環境での配置を保証しません。GTK 3／X11のSYSTEM表示では、装飾付き未確定文字とシステム候補を使用できます。[4系の相対座標の扱い](https://github.com/fcitx/fcitx/blob/4.2.9.9/src/ui/kimpanel/kimpanel.c#L488)

両世代とも、有効な候補通知を受信するまでは、所有者をNONE、理由をUNCONFIRMEDで報告します。SYSTEMはシステム表示への委譲を表し、ポップアップが実際に表示されたことを保証する値ではありません。fcitx 5の4系互換経路には文字列パネルを提供しない実装があるため、通常はAUTOか5系の明示指定を使ってください。[5系の4系互換実装](https://github.com/fcitx/fcitx5/blob/5.1.12/src/frontend/fcitx4frontend/fcitx4frontend.cpp#L235)

接続先が停止すると未確定文字と候補を消去し、再接続しても古い表示や送信済みキーを再利用しません。4系のフォーカス喪失時の確定は、サーバーの設定に従います。パネルに含まれる操作案内を確定本文として送ることはありません。

X11でfcitx 5のクライアント候補を使うには、デーモン側の`FCITX_X11_USE_CLIENT_SIDE_UI=True`設定が必要な場合があります。Waylandでも、コンポジターやパネル環境によって能力が調整されます。候補が表示されない場合は、設定と候補所有者の診断を確認してください。[fcitx 5の候補表示能力の調整](https://github.com/fcitx/fcitx5/blob/5.1.7/src/frontend/dbusfrontend/dbusfrontend.cpp)、[公式Wayland設定ガイド](https://fcitx-im.org/wiki/Using_Fcitx_5_on_Wayland)

## アプリケーションの編集文書を渡す

端末の表示内容は通信先が管理するため、IMEから要求された削除や置換を、そのまま端末セルの変更で代用しません。
アプリが本文を所有するエディター用途では、表示先のeditable documentへ、本文の一部、カーソル、選択、文書IDとrevisionを公開します。

`boxrig_ui_editable_document_publish_context()`は周辺文字列をコピーします。
位置はUTF-8の絶対バイト位置で、cursorとanchorは文字境界に置きます。
未確定文字は本文へ含めません。
本文を変えず選択だけを変える場合でも、入力状態のgenerationは別に更新されます。

小さい実行例は[step-by-stepの編集文書](./step-by-step/step-by-step-ui.md)にあります。
[公開ヘッダー](../../include/boxrig/ui/editable.h)には、容量、境界、失敗時に前の状態を保持する条件が記載されています。

## 確定と再変換を扱う

同期編集では、アプリの所有者コールバックが要求された置換を一度だけ適用し、新しいrevisionを受け入れます。
失敗時には本文を変更しません。
コールバック内でI/O待機、ネストしたイベントループ、表示先の変更や破棄を行わないでください。

再変換では、対象の文書ID、revision、選択範囲を固定してから要求を進めます。
返された置換要求をアプリが取り出し、適用、拒否、古い要求のいずれかで完了させます。
途中で本文や入力状態が変わった要求を、現在の文書へ適用しないでください。

通常端末への送信と編集文書の置換は、どちらへ確定を渡すかが異なります。
一つの確定を両方へ配送しないことが必要です。

Win32では、周辺文書の公開や選択範囲の再変換を接続しても、通常のIME確定はPTYへ送ります。
共通の同期編集コールバックを登録しただけでは、この確定先は切り替わりません。
各入力経路の違いは[IMEの対応表](./reference/platform-support.md#ime通知クリップボードアクセシビリティ)で確認してください。

## 環境ごとの制約を調べる

周辺文字列を取得できること、同期編集できること、再変換できることは、それぞれ別の能力です。
ネイティブAPI、IME、入力経路、アプリ側の編集所有者がそろっているかを確認してください。

対応条件の入口は[互換性](./compatibility.md#imeの周辺文字列と同期編集)、実行時の選び方は[付属ターミナル](./boxrig_term.md)です。
処理待ちの入力やクリップボードを保持したまま終了する場合は、[寿命と終了](./architecture.md#所有権と終了)に従って通知先を切り離します。

[文書目次](./index.md)
