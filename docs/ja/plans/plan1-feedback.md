R1・R2・I1・K1・W1は、boxrigの機能として取り込むのが妥当です。P1はPOSIXの起動診断を追加し、Win32では既存の診断APIを利用したうえで非同期起動の不足を別に扱います。B1はC++対応の不備修正と共有ビルドの追加に分け、A1はGTK3・GTK4の公式ウィジェットで進めることを推奨します。

調査対象のコードは`8124d19`です。要件書の基準版`5ebe642`との差分はREADMEのみで、指摘された不足は現行コードにも当てはまります。2026年10月14日の追加調査では、K1のGNOME Terminal相当の選択肢、P1のWin32対応、A1のGTK4対応を検討しました。以下のAPI追加は設計案であり、実装済みの機能とは区別します。

| 項目 | 判断 | boxrigで担う範囲 |
|---|---|---|
| R1 任意文字列のregex | 採用 | 端末・GUIの生成を必要としない、既存認識器と共通の照合API |
| R2 キャプチャ | 採用。ただし設計検証を先行 | 番号・名前付きグループ、結果の寿命、画面認識との一貫性 |
| I1 入力禁止 | 採用 | 共通UIの入力制御と、各frontendのIME・clipboard・paste停止処理 |
| K1 実行中のキー設定変更 | 採用 | Enter・矢印mappingに加え、Backspace・Deleteの送信方式を個別に選び、一括交換するAPI |
| W1 行列数変更要求 | 採用 | coreで解析し、型付きhost itemとして通知 |
| P1 PTY起動結果 | 採用 | POSIXの起動結果通知を追加。Win32は既存の診断・終了待機を利用し、起動そのものの非同期化を別段階で検証 |
| B1 C++ヘッダー | 不備として修正 | 公開ヘッダーを通常includeしてC ABIへリンクできる状態 |
| B1 Linux共有ビルド | 採用推奨 | 既存のライブラリ分割に沿った公式の共有ビルド・配布 |
| A1 アクセシビリティ | boxrig側で対応 | 共通の本文・位置対応を基に、GTK3のAtkTextとGTK4のGtkAccessibleTextへ公開。必要範囲は実測で確定 |

取り込み方では、以下の点を要件書より具体化する必要があります。

R1・R2は、既存regexを共通部品へ整理して公開する形が適切です。採用する理由は、画面認識と設定検証、URIや受信文字列の照合で、文法・診断・一致規則を統一できることにあります。

現在の[内部regex API](/home/kouji/Projects/boxrig/src/core/regex.h:17)は、デコード済みのUnicodeスカラー列と書記素境界を受け取り、容量設定にも画面認識器の型を使っています。そのまま公開するには端末依存が残ります。公開入口を`include/boxrig/regex.h`へ分け、長さ付きUTF-8、開始バイト位置、独立した容量設定、分割実行を提供する案を推奨します。コンパイル結果は不変オブジェクトとして照合器と画面認識器で共有し、所有権を明記します。

R2では、結果型へ配列を足すだけでは不十分です。現在の[NFA処理](/home/kouji/Projects/boxrig/src/core/regex.c:1455)は、同じ状態へ到達した経路をまとめます。キャプチャを扱うには、異なる経路から得られる部分一致の選択規則が必要です。全体の最左最長に加え、同長の場合の優先、反復グループの取得値、未参加と空文字の区別を先に決めるべきです。

画面側は、認識時に確定したキャプチャを`match_id`とrevisionで取得する方式が適しています。全体一致の両端は現在どおり書記素境界に限定し、キャプチャ内部はUTF-8バイト範囲で表す案が自然です。結合文字の途中にある部分一致を、既存の`boxrig_text_range`へ無理に変換せずに済みます。取得済みの不変コピーは更新後も読めるようにし、現在の画面に対応する結果を取得・適用するときに`STALE`を判定します。

また、現在の[検索処理](/home/kouji/Projects/boxrig/src/core/regex.c:1644)は、候補開始位置ごとに走査をやり直します。例えば長い`a`列に対する`a+b`の不一致では、総処理量が大きくなる構造です。公開化の完了条件には、分割実行だけでなく、不一致入力の総処理量と、キャプチャ追加による保持量の検証も必要です。最初の公開版は現在の限定文法・空一致禁止を維持し、番号・名前付きキャプチャを追加する範囲でよいと考えます。

I1は、共通UIで契約を定め、各frontendへ実装するのが適切です。GTK3にはキー経路のほか、[IME確定から直接送信する経路](/home/kouji/Projects/boxrig/src/gtk3/input.c:224)や非同期clipboard応答があります。キーrouterだけでは入力を止められません。

設定単位はコンポーネントとし、禁止時に入力世代を更新して、保留キー・IME確定・clipboard応答・未開始pasteを無効化します。選択、コピー、履歴移動、描画は継続させます。開始済みpasteには終端処理が必要で、[現在のcore API](/home/kouji/Projects/boxrig/src/core/input.c:1453)でも終端の投入が送信容量不足で待たされます。そのため、単純なbool setterより、禁止要求と停止完了を区別できる契約が適しています。

停止完了の境界は、必要な終端などをcoreへ投入するところまでとします。すでにcoreや通信路へ渡ったデータの排出・破棄、接続世代の切り替えはelder-termsが担当します。端末応答も同じ出力経路を通るため、送信全体の停止で代用すると要件を満たせません。

K1は、GNOME Terminalの「互換性」設定に相当する選択肢までboxrigで提供できます。現行GNOME TerminalはBackspaceとDeleteに、それぞれAutomatic、Control-H、ASCII DEL、Escape Sequence、TTY Eraseを選べます。対象は端末へ送る消去キーであり、コピーやタブ操作などのアプリ用ショートカットとは別の設定です。[GNOME Terminalの設定UI（調査時の固定版）](https://github.com/GNOME/gnome-terminal/blob/1079728b68a963d0fcd5602580aeead41a0f1249/src/terminal-profile-editor.ui#L738)

GNOME Terminalの既定値はBackspaceがASCII DEL、DeleteがEscape Sequenceです。一方、VTEのAutomaticはBackspaceでTTY設定の取得を試み、取得できなければBS、Deleteでは削除シーケンスを使います。設定の既定値とAutomaticの解決規則は別です。boxrigでは要件書の方針どおり、Autoを現行boxrigの規則に合わせ、GNOME Terminalと同じ選択肢を用意します。[GNOME Terminalの設定schema](https://github.com/GNOME/gnome-terminal/blob/1079728b68a963d0fcd5602580aeead41a0f1249/src/org.gnome.Terminal.gschema.xml#L338)、[VTEのEraseBinding](https://gnome.pages.gitlab.gnome.org/vte/gtk4/enum.EraseBinding.html)、[VTEのキー生成](https://github.com/GNOME/vte/blob/56bad78b8ed59716f14ccc0ef678f32df4f9dc7b/src/vte.cc#L5149)

追加する設定型は、たとえば`boxrig_erase_binding`とし、Backspace用とDelete用に独立した値を持たせます。次の表は修飾キーがない場合の送信案です。名前は設計上の仮称です。

| 選択肢 | Backspace | Delete |
|---|---|---|
| `AUTO` | 現行どおりDEL、`0x7f` | 現行どおり`ESC [ 3 ~` |
| `ASCII_BACKSPACE` | BS、`0x08` | BS、`0x08` |
| `ASCII_DELETE` | DEL、`0x7f` | DEL、`0x7f` |
| `DELETE_SEQUENCE` | `ESC [ 3 ~` | `ESC [ 3 ~` |
| `TTY` | 通信路から渡された現在の消去文字 | 通信路から渡された現在の消去文字 |

これらをEnterのCR／LF／CRLF／端末モード追従、既存の矢印mappingとともに、一つの入力設定として検証・コピーして交換します。生成時と実行中に同じ設定型・検証規則を使い、取得APIも用意します。不正な値やコピー失敗では旧設定を保持し、画面・履歴・選択・端末モードを再生成しません。coreのAPIはGTKに依存しないため、GTK3・GTK4・Win32で共通に使えます。設定画面、プロファイルの保存、TRS-80用の4方向の値はアプリ側が用意します。

TTYの消去文字はcoreだけでは取得できません。POSIX補助層で`tcgetattr()`の`VERASE`を取得し、通信路を持つホストが「取得できたか」と1バイトの値をcoreへ渡す構成を推奨します。POSIX PTYでは対象キーの処理時に取得し、起動時の値を固定したり、タイマーで監視したりしません。これにより、後から行われた`stty erase`の変更も次の入力へ反映できます。NULや端末側の消去機能の無効値を含め、取得した値は1バイトとして扱い、未取得とは区別します。VTEもTTY設定をキー生成時に参照する実装です。[VTEのTTY参照処理](https://github.com/GNOME/vte/blob/56bad78b8ed59716f14ccc0ef678f32df4f9dc7b/src/vte.cc#L5172)

SSH・Telnet・シリアルでは、接続層が消去文字を把握できる場合にだけ渡します。Win32では、[現行PTY API](../../../include/boxrig/win32/pty.h)からTTYの消去文字を取得できないため、未取得として扱います。TTYが未取得の場合は当該キーのAutoへ戻す案です。ホストは取得状態を参照でき、設定画面でTTYが使用できないことを示せます。これなら保存済み設定を別の通信路で開いても、キーが無条件に失われることを避けられます。接続を切り替える際にはTTY情報を破棄し、前の接続の値を流用しません。

修飾キーの扱いも設定の一部として明記します。1バイトのBS／DELではCtrlで両者を反転し、AltではESCを前置する案が、現行boxrigのAutoとVTEの挙動に沿います。TTYが別の文字ならCtrlでBS／DELへ変換しません。削除シーケンスでは既存の修飾付きDeleteと同じ`CSI 3 ; modifier ~`を使い、Alt用ESCを重ねません。Shiftなどの修飾は既存の規則を引き継ぎます。矢印の任意バイトmappingは、[現在の公開契約](../../../include/boxrig/terminal.h)どおり完成済みのバイト列を送り、これらの補正を加えません。消去キーの選択肢を追加するために、全キーの任意mappingへ拡張する必要はありません。

[現在のキー生成](../../../src/core/input.c)はKitty処理を先に行い、その後にlegacy mappingへ進みます。この順序を維持し、新しい消去キー設定もKittyが処理しなかった入力へ適用します。keypadのDelete別名にも同じ設定を適用し、application keypadとKittyの既存規則を保ちます。legacyのpress・repeatは送信し、releaseは送信しません。

coreはfrontendの保留イベントや物理キーの押下状態を把握していません。coreでは呼び出し境界で設定を一括交換し、UIでは保留入力・押下系列の境界まで待つか、`BUSY`を返す契約にします。変更前のpressに対応するreleaseや保留IMEを、新しい設定で別のキーとして再解釈しないことが必要です。検証には両キーの全選択肢、Ctrl／Alt、keypad、Kitty push／pop、設定交換失敗、押下途中の交換、TTYの変更・無効・未取得・接続切り替えを含め、送信先が受け取るバイトで判定します。

W1は既存のhost queueへ追加できます。ここでは「0」と「省略」を同じ値へ正規化しないことが必要です。xterm仕様では、省略は現在寸法の維持、0はディスプレイ寸法の使用を意味します。[XTerm Control Sequences](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html)

追加する要求型では、各軸を「維持・明示値・ディスプレイに合わせる」で表現する案を推奨します。coreは要求を通知し、ホストが許可と実寸を決め、既存のlayout経路へ渡します。通常のサイズ変更要求自体に応答を追加する必要はなく、初期実装はタイトル通知と同様の完了応答不要なhost itemで十分です。layoutの完了管理には既存のrevisionを使えます。

P1はPOSIXとWin32の両方を対象にしますが、現行APIの不足は異なります。POSIXでは公開PTY APIの診断不足として対応すべきです。[現在の実装](/home/kouji/Projects/boxrig/src/posix/pty.c:35)を使った検証では、次の両方が`spawn=0、wait=0、exit=127`になりました。

- 存在しないコマンドの起動
- 正常に起動して終了値127を返すコマンド

POSIXには、起動結果用の待機可能FDと、非ブロッキングで状態・失敗段階・errnoを取得するAPIを追加する案が適しています。起動結果と子プロセスの終了結果は別に扱い、子の回収者を一つに定めます。GTK callback形式や接続表示までboxrigに含める必要はありません。

Win32にはすでに[`boxrig_win32_pty_spawn_with_config()`](../../../include/boxrig/win32/pty.h)があります。PTYが作れなかった場合にも、呼び出し元が所有する診断構造体へ失敗段階・Win32エラー・有効なHRESULT・ConPTY backendの試行履歴を返す契約です。`boxrig_win32_pty_process_handle()`は、起動した子プロセスの終了を待てるHANDLEを公開しています。

| 観点 | 現行POSIX | 現行Win32 | 取り込み方 |
|---|---|---|---|
| 起動時の失敗理由 | 親側の失敗はerrno、子のexec失敗は終了値127のみ | 段階・Win32エラー・HRESULTを診断へ保存 | POSIXを補完し、Win32の診断形式は利用する |
| 起動失敗と通常終了127の区別 | 子のexec失敗では区別できない | `CreateProcessW()`の失敗と、生成後の終了を区別できる | 共通の観測項目にし、OS固有のエラー値を失わない |
| 起動結果を非同期に待つ入口 | 専用FDなし | spawnは同期呼び出しであり、起動完了用HANDLEなし | 診断の有無とは別に設計する |
| 子プロセスの終了監視 | PID取得・waitを提供 | 待機可能なprocess HANDLEを提供 | Cardioなどのイベントループへ接続する |

Win32の取り込みでは、同じ情報を返す診断APIをもう一組追加せず、既存APIを使うドライバーと受け入れ試験を整えます。[現在の実装](../../../src/win32/pty.c)は`CreateProcessW()`の失敗時にエラーを保存し、後始末後も診断と`GetLastError()`へ反映します。[既存テスト](../../../tests/win32/test_pty_runtime.c)にも、子の生成失敗と、その場合にbackendを再試行しないことの検証があります。

起動成功の意味は、アプリケーションが利用可能になったことまで含めません。Win32では`CreateProcessW()`が成功した後もDLLのロード・初期化に失敗して終了することがあります。また、`cmd.exe /c`で不存在コマンドを実行する場合は、シェルの生成成功と、その内部コマンドの失敗を分けます。成功直後の終了を起動失敗へ一律に置き換えず、起動結果と終了結果をそれぞれ保持します。[CreateProcessWの戻り値と初期化の契約](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)

終了監視には、公開HANDLEをCardioの`from_win32_handle()`へ渡せます。この入口はHANDLEがsignaledになるまで非同期に待つもので、HANDLEの所有権を取得しません。待機解除とI/Oの停止を終えてからPTYを破棄する契約が必要です。終了後に`GetExitCodeProcess()`で値を取得し、値259だけで実行中と判断しません。259は`STILL_ACTIVE`と同じ数値でも、実際の終了値になり得るためです。boxrigのC APIは診断と待機対象を公開し、Cardioへの接続はC++20のドライバーやホストに置きます。[Cardioの公式説明](../../../deps/cardio/README_ja.md)、[CardioのAPIコメント](../../../deps/cardio/include/cardio.h)、[GetExitCodeProcess](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getexitcodeprocess)

Win32で残る不足は、起動呼び出しそのものの非同期化と起動中の中止です。子プロセスのHANDLEで待てるのは終了であり、`CreatePseudoConsole()`や`CreateProcessW()`の呼び出しを非同期にはしません。同期spawnをcoroutine内で呼ぶだけでもGUIは停止し得ます。POSIXの診断FD追加も、PTY準備やforkなどの親側処理をすべて非同期化する保証ではありません。

初期段階では、起動診断と終了監視をPOSIX・Win32のドライバーで観測できるようにします。その後、起動・失敗後の後始末・中止を含む非同期操作を独立した段階として検証します。操作の要求、完了通知、診断の取得、取消要求、資源回収の完了を区別し、取り消された操作の遅い完了を新しい接続へ適用しない契約にします。同期OS関数をどこで実行するかは、既存ホストの実行基盤を調べ、GUIを止めずに回収まで行えるドライバーで方式を確定します。Cardioによる完了待機だけで解決済みとせず、ワーカースレッドやポーリングを安易に追加しません。この段階が完了するまでは、Win32の完全な非同期起動を対応済みと扱いません。

ConPTYの後始末も、この検証に含めます。現行の通信HANDLEは同期I/Oであり、CardioのOVERLAPPED用APIで包むだけでは非同期I/Oへ変わりません。また、Windows 11 24H2より前の`ClosePseudoConsole()`は同期的に待つことがあり、出力の排出または切断の順序が必要です。現行boxrigのdestroyが出力を先に閉じる順序を維持し、出力を回収するclose経路と、出力を破棄するdestroy経路の両方を確認します。[MicrosoftのConPTY I/O説明](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session)、[ClosePseudoConsole](https://learn.microsoft.com/en-us/windows/console/closepseudoconsole)

P1のWin32受け入れ試験には、存在しない実行ファイル、アクセス拒否、不正な実行形式、通常終了127・259、生成直後の終了、ConPTY backendの失敗とfallback、子を二重に生成しないこと、診断値とHANDLEの寿命を含めます。非同期起動の段階では中止と完了の競合、接続世代、GUIの応答、後始末を追加します。MinGWの32／64bitビルドと既存のAPI差し替えテストを使い、実ConPTYはシステム版・同梱版で確認します。Wineで検証できない部分は`agent-rover`で実Windowsを使い、Wine固有の修正を取り込む前提にはしません。

B1のC++対応は共有化に先立って直せます。[POSIXヘッダー](/home/kouji/Projects/boxrig/include/boxrig/posix.h:7)をC++から通常includeし、Cでコンパイルした実装へリンクすると、名前修飾の違いによるリンク失敗を再現しました。GTK3 widget自身には`G_BEGIN_DECLS`がありますが、先にincludeする共通UIやsessionなどまで保護されるわけではありません。公開ヘッダー全体を対象にC99／C++20の利用側テストを設けるべきです。

共有ビルドは、core・UI・session・POSIX・GTK3という既存の分割に沿って追加します。PIC化に加え、公開symbolの明示、SONAME、pkg-config、インストール先でのロード確認が必要です。既存のprivate codecのhidden visibilityは維持します。また、[Makefile](/home/kouji/Projects/boxrig/Makefile:21)などには`build/`固定の生成先があるため、外部build directory対応も一緒に整理します。上流で提供すれば、elder-terms側でソース一覧や依存関係を維持する負担を減らせます。

A1はGTK3・GTK4の両方で取り込むべきです。GTK3の[現在のテスト](/home/kouji/Projects/boxrig/tests/gtk3/test_gtk3_widget.c:2268)は、accessibleが`AtkText`ではないことを確認しています。GTK4も[現行ウィジェット](../../../src/gtk4/terminal_widget.c)がterminal roleを設定するだけで、`GtkAccessibleText`を実装していません。[GTK4のテスト](../../../tests/gtk4/test_gtk4_widget.c)もroleの確認にとどまります。

本文と位置の対応は共通UIへまとめ、GTK3では`AtkText`、GTK4では`GtkAccessibleText`として公開する構成を推奨します。coreの本文・履歴・カーソル情報と、各表示の選択・スクロール・配置情報から、アクセシビリティ向けの不変snapshotを作ります。本文の取得範囲、文字offsetと端末セルの対応、変更範囲の計算は共通にし、GTK固有のインターフェイスと通知だけを各frontendに置きます。GTK4へATKを持ち込んだり、両frontendが制御列を解析し直したりする必要はありません。

GTK4の現在の最低要件は、[Makefile](../../../Makefile)と[pkg-config定義](../../../pkgconfig/boxrig-gtk4.pc)で4.14です。`GtkAccessibleText`も4.14から使えるため、本文・caret・選択・変更通知の対応だけなら最低要件を引き上げずに実装できます。ただし、現行のGTKドキュメントには後の版で追加された操作も含まれます。[GtkAccessibleText](https://docs.gtk.org/gtk4/iface.AccessibleText.html)

| GTK4の版 | 利用できる主な契約 | boxrigへの取り込み |
|---|---|---|
| 4.14以降 | 本文・指定位置周辺の本文、caret位置、選択範囲、文字属性の取得。本文・caret・選択の変更通知 | 最初の実装範囲とする |
| 4.16以降 | 文字範囲から表示領域を求める`get_extents`、座標から文字位置を求める`get_offset` | 共通の位置対応を利用し、版の条件を付けた追加対応として検証する |
| 4.22以降 | 支援技術からの`set_caret_position`、`set_selection` | 表示上の選択操作と、子プロセスが管理する端末カーソルを区別して採否を決める |

版ごとの根拠は、[get_extents](https://docs.gtk.org/gtk4/vfunc.AccessibleText.get_extents.html)、[get_offset](https://docs.gtk.org/gtk4/vfunc.AccessibleText.get_offset.html)、[set_caret_position](https://docs.gtk.org/gtk4/vfunc.AccessibleText.set_caret_position.html)、[set_selection](https://docs.gtk.org/gtk4/vfunc.AccessibleText.set_selection.html)です。4.14の最低環境でもビルド・実行できる範囲を先に完成させ、新しい版でだけ使える操作を無条件に呼びません。端末カーソルの移動をテキストエディターのcaret移動と同一視したり、そのための入力を子へ勝手に送ったりしない契約にします。

ATKとGtkAccessibleTextの位置は文字単位なので、UTF-8バイト位置、書記素境界、端末列との変換が必要です。全角文字の継続セルを余分な文字として数えず、結合文字・絵文字を含む本文とoffsetを一致させます。soft wrapと実改行、空白セル、履歴の欠落、代替画面、reflow後の位置も定義します。複数の表示で同じterminalを共有しても、選択や表示領域を混同しないことが必要です。IMEの未確定文字列は、受信して確定した端末本文に混ぜません。[Atk.Text.get_text](https://docs.gtk.org/atk/method.Text.get_text.html)、[GtkAccessibleText.get_contents](https://docs.gtk.org/gtk4/vfunc.AccessibleText.get_contents.html)

変更通知は本文・caret・選択の変更に対応させます。GTK4の削除通知は旧本文を取り除く前、挿入通知は新本文を反映した後に出す契約です。そのため、共通snapshotにも通知中に参照できる旧本文と新本文が必要です。点滅やアニメーションだけで同じ文章を再通知しないようにします。切断やwidget破棄の途中で遅い通知が届く場合も検証します。[GtkAccessibleText.update_contents](https://docs.gtk.org/gtk4/method.AccessibleText.update_contents.html)

実装順は、共通の本文snapshotとGTK3のAtkTextを一つの実行可能な成果物にし、次にGTK4の4.14対応を接続する案です。受け入れ試験ではAT-SPIのText取得、caret、選択、挿入・削除イベントの内容と順序を確認します。通常画面・履歴移動・代替画面・reflow・結合文字を含め、GTK3とGTK4で同じ本文の意味になることを検証します。インターフェイスが存在することだけでは完了としません。

VTEとのAT-SPI比較とOrcaによる読み上げは、今回まだ実測していません。先にGTK3・GTK4それぞれの現行VTEを観測し、公開する本文の範囲と通知の粒度を確定します。読み上げに4.16以降の位置取得が必要なら、その対応も移行の完了条件へ加え、4.14で達成できる範囲との違いを明記します。GTK3での結果だけからGTK4の同等性を判断しません。

elder-termsに残す範囲については、要件書の分担が適切です。接続管理、通信方式、iconv変換、ログ、マクロの実行管理、テンプレート展開、リンク先の検証、速度付きテキスト送信はアプリ側で扱います。通常のフォント・レイアウト操作にも追加APIは不要です。非同期SSH resizeも、通信先の処理完了後にcoreを更新し、[既存の完了API](/home/kouji/Projects/boxrig/include/boxrig/gtk3/terminal_widget.h:307)へ同じrevisionを返す構成で対応できます。

実装は、次の単位で進めることを推奨します。各段階で実行可能なドライバーを保ち、別々にコミットできる構成です。

| 段階 | 成果物 | 完了条件 |
|---|---|---|
| 1 | C++公開ヘッダー修正 | 通常includeしたC99／C++20の利用側からリンク・実行できる |
| 2 | R1の独立regex APIとCLI | GUIなしで照合でき、NUL、不正UTF-8、診断位置、容量制限、分割再開を検証できる |
| 3 | R2のキャプチャと認識器統合 | 同じ文脈で同じ結果が得られ、反復・未参加・結合文字・古いrevisionを検証できる |
| 4 | K1の設定交換と4種類の消去キー設定 | Auto・BS・DEL・削除シーケンスを両キーへ設定でき、画面を保持した変更、Ctrl／Alt、keypad、Kitty、設定交換の境界を送信バイトで確認できる |
| 5 | K1のTTY設定連携 | POSIXドライバーで消去文字の変更を追従でき、無効・未取得・接続切り替えを検証できる。Win32を含む他の通信路でもAutoへの復帰を確認できる |
| 6 | I1の入力禁止 | 遅延IME・clipboard・pasteが再開後へ漏れず、選択コピーと端末応答が継続する |
| 7 | W1のhost通知 | 分割受信、0と省略、キュー満杯、ホスト拒否、既存layoutとの接続を検証できる |
| 8 | P1の起動診断と終了監視ドライバー | POSIXでexec失敗と通常終了127を区別できる。Win32では既存の段階・エラー・HRESULTを取得し、通常終了127・259と区別できる。回収とFD／HANDLEの寿命を確認できる |
| 9 | P1の非同期起動・中止ドライバー | 方式を検証して確定し、起動・失敗時の後始末・中止中もGUIが応答する。完了との競合で二重生成・二重回収・次接続への通知混入がなく、資源回収まで完了できる |
| 10 | Linux共有ビルド | クリーン・増分・外部ディレクトリビルドと、配布先からの実ロードが成功する |
| 11 | A1の共通snapshotとGTK3対応 | 実行可能なGTK3ドライバーで、現行VTEとの比較から確定した本文・選択・通知・読み上げの条件を満たす |
| 12 | A1のGTK4対応 | 最低版4.14で本文・caret・選択・通知が機能する。GTK4版VTEとの比較条件を満たし、後の版に依存する操作を区別して検証できる |

A1の比較観測と、P1の起動・後始末をGUIから非同期に扱う方式の検証は早期に行い、実装範囲を先に確定します。P1の段階8が完了しても、段階9の非同期起動・中止まで完了したとは扱いません。A1の4.16／4.22以降の追加操作は、比較観測で必要性を判断し、必要なら独立した実装段階を追加します。各実装ではRED／GREENを確認し、全項目の完了時に全体テストを実行します。

この調査の完了条件は、K1の設定候補と適用順、P1のOS別の現状と不足、A1のGTK版別の対応範囲を根拠付きで示し、取り込み方と受け入れ条件を保存することです。本書への反映により、この分析作業の完了条件を満たしました。

初回調査ではコードの照合とPOSIX P1・B1の再現確認を行い、追加調査では現行コード、公開APIコメント、GNOME／VTE・GTK・Microsoftの公式資料を照合しました。今回の変更はこの文書のみです。実装、Win32実機試験、AT-SPI・読み上げの比較実測、全体テスト、移行後の結合試験は行っていません。文書はビルド入力ではないため、今回の変更に対するビルド確認は不要です。
